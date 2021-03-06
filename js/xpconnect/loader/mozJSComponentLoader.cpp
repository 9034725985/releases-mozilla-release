/* -*- Mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "mozilla/Attributes.h"

#ifdef MOZ_LOGGING
#define FORCE_PR_LOG
#endif

#include <stdarg.h>

#include "prlog.h"
#ifdef ANDROID
#include <android/log.h>
#endif
#ifdef XP_WIN
#include <windows.h>
#endif

#include "nsCOMPtr.h"
#include "nsAutoPtr.h"
#include "nsICategoryManager.h"
#include "nsIComponentManager.h"
#include "mozilla/Module.h"
#include "nsIFile.h"
#include "nsIServiceManager.h"
#include "nsISupports.h"
#include "mozJSComponentLoader.h"
#include "mozJSLoaderUtils.h"
#include "nsIJSRuntimeService.h"
#include "nsIJSContextStack.h"
#include "nsIXPConnect.h"
#include "nsCRT.h"
#include "nsMemory.h"
#include "nsIObserverService.h"
#include "nsIXPCScriptable.h"
#include "nsString.h"
#include "nsIScriptSecurityManager.h"
#include "nsIURI.h"
#include "nsIFileURL.h"
#include "nsIJARURI.h"
#include "nsNetUtil.h"
#include "nsDOMBlobBuilder.h"
#include "jsprf.h"
#include "nsJSPrincipals.h"
// For reporting errors with the console service
#include "nsIScriptError.h"
#include "nsIConsoleService.h"
#include "nsIStorageStream.h"
#include "nsIStringStream.h"
#if defined(XP_WIN)
#include "nsILocalFileWin.h"
#endif
#include "xpcprivate.h"
#include "xpcpublic.h"
#include "nsIResProtocolHandler.h"
#include "nsContentUtils.h"
#include "WrapperFactory.h"

#include "mozilla/scache/StartupCache.h"
#include "mozilla/scache/StartupCacheUtils.h"
#include "mozilla/Omnijar.h"
#include "mozilla/Preferences.h"

#include "jsdbgapi.h"


using namespace mozilla;
using namespace mozilla::scache;
using namespace xpc;

// This JSClass exists to trick silly code that expects toString()ing the
// global in a component scope to return something with "BackstagePass" in it
// to continue working.
static JSClass kFakeBackstagePassJSClass =
{
    "FakeBackstagePass",
    0,
    JS_PropertyStub,
    JS_PropertyStub,
    JS_PropertyStub,
    JS_StrictPropertyStub,
    JS_EnumerateStub,
    JS_ResolveStub,
    JS_ConvertStub
};

static const char kJSRuntimeServiceContractID[] = "@mozilla.org/js/xpc/RuntimeService;1";
static const char kXPConnectServiceContractID[] = "@mozilla.org/js/xpc/XPConnect;1";
static const char kObserverServiceContractID[] = "@mozilla.org/observer-service;1";
static const char kJSCachePrefix[] = "jsloader";

/* Some platforms don't have an implementation of PR_MemMap(). */
#ifndef XP_OS2
#define HAVE_PR_MEMMAP
#endif

/**
 * Buffer sizes for serialization and deserialization of scripts.
 * FIXME: bug #411579 (tune this macro!) Last updated: Jan 2008
 */
#define XPC_SERIALIZATION_BUFFER_SIZE   (64 * 1024)
#define XPC_DESERIALIZATION_BUFFER_SIZE (12 * 8192)

#ifdef PR_LOGGING
// NSPR_LOG_MODULES=JSComponentLoader:5
static PRLogModuleInfo *gJSCLLog;
#endif

#define LOG(args) PR_LOG(gJSCLLog, PR_LOG_DEBUG, args)

// Components.utils.import error messages
#define ERROR_SCOPE_OBJ "%s - Second argument must be an object."
#define ERROR_NOT_PRESENT "%s - EXPORTED_SYMBOLS is not present."
#define ERROR_NOT_AN_ARRAY "%s - EXPORTED_SYMBOLS is not an array."
#define ERROR_GETTING_ARRAY_LENGTH "%s - Error getting array length of EXPORTED_SYMBOLS."
#define ERROR_ARRAY_ELEMENT "%s - EXPORTED_SYMBOLS[%d] is not a string."
#define ERROR_GETTING_SYMBOL "%s - Could not get symbol '%s'."
#define ERROR_SETTING_SYMBOL "%s - Could not set symbol '%s' on target object."

void
mozJSLoaderErrorReporter(JSContext *cx, const char *message, JSErrorReport *rep)
{
    nsresult rv;

    /* Use the console service to register the error. */
    nsCOMPtr<nsIConsoleService> consoleService =
        do_GetService(NS_CONSOLESERVICE_CONTRACTID);

    /*
     * Make an nsIScriptError, populate it with information from this
     * error, then log it with the console service.  The UI can then
     * poll the service to update the Error console.
     */
    nsCOMPtr<nsIScriptError> errorObject =
        do_CreateInstance(NS_SCRIPTERROR_CONTRACTID);

    if (consoleService && errorObject) {
        uint32_t column = rep->uctokenptr - rep->uclinebuf;

        const PRUnichar* ucmessage =
            static_cast<const PRUnichar*>(rep->ucmessage);
        const PRUnichar* uclinebuf =
            static_cast<const PRUnichar*>(rep->uclinebuf);

        rv = errorObject->Init(
              ucmessage ? nsDependentString(ucmessage) : EmptyString(),
              NS_ConvertASCIItoUTF16(rep->filename),
              uclinebuf ? nsDependentString(uclinebuf) : EmptyString(),
              rep->lineno, column, rep->flags,
              "component javascript");
        if (NS_SUCCEEDED(rv)) {
            rv = consoleService->LogMessage(errorObject);
            if (NS_SUCCEEDED(rv)) {
                // We're done!  Skip return to fall thru to stderr
                // printout, for the benefit of those invoking the
                // browser with -console
                // return;
            }
        }
    }

    /*
     * If any of the above fails for some reason, fall back to
     * printing to stderr.
     */
#ifdef DEBUG
    fprintf(stderr, "JS Component Loader: %s %s:%d\n"
            "                     %s\n",
            JSREPORT_IS_WARNING(rep->flags) ? "WARNING" : "ERROR",
            rep->filename, rep->lineno,
            message ? message : "<no message>");
#endif
}

static JSBool
Dump(JSContext *cx, unsigned argc, jsval *vp)
{
    JSString *str;
    if (!argc)
        return true;

    str = JS_ValueToString(cx, JS_ARGV(cx, vp)[0]);
    if (!str)
        return false;

    size_t length;
    const jschar *chars = JS_GetStringCharsAndLength(cx, str, &length);
    if (!chars)
        return false;

    NS_ConvertUTF16toUTF8 utf8str(reinterpret_cast<const PRUnichar*>(chars));
#ifdef ANDROID
    __android_log_print(ANDROID_LOG_INFO, "Gecko", "%s", utf8str.get());
#endif
#ifdef XP_WIN
    if (IsDebuggerPresent()) {
      OutputDebugStringW(reinterpret_cast<const PRUnichar*>(chars));
    }
#endif
    fputs(utf8str.get(), stdout);
    fflush(stdout);
    return true;
}

static JSBool
Debug(JSContext *cx, unsigned argc, jsval *vp)
{
#ifdef DEBUG
    return Dump(cx, argc, vp);
#else
    return true;
#endif
}

static JSBool
Atob(JSContext *cx, unsigned argc, jsval *vp)
{
    if (!argc)
        return true;

    return xpc::Base64Decode(cx, JS_ARGV(cx, vp)[0], &JS_RVAL(cx, vp));
}

static JSBool
Btoa(JSContext *cx, unsigned argc, jsval *vp)
{
    if (!argc)
        return true;

    return xpc::Base64Encode(cx, JS_ARGV(cx, vp)[0], &JS_RVAL(cx, vp));
}

static JSBool
File(JSContext *cx, unsigned argc, jsval *vp)
{
    nsresult rv;

    if (!argc) {
        XPCThrower::Throw(NS_ERROR_UNEXPECTED, cx);
        return false;
    }

    nsCOMPtr<nsISupports> native;
    rv = nsDOMMultipartFile::NewFile(getter_AddRefs(native));
    if (NS_FAILED(rv)) {
        XPCThrower::Throw(rv, cx);
        return false;
    }

    nsCOMPtr<nsIJSNativeInitializer> initializer = do_QueryInterface(native);
    NS_ASSERTION(initializer, "what?");

    rv = initializer->Initialize(nullptr, cx, nullptr, argc, JS_ARGV(cx, vp));
    if (NS_FAILED(rv)) {
        XPCThrower::Throw(rv, cx);
        return false;
    }

    nsXPConnect* xpc = nsXPConnect::GetXPConnect();
    if (!xpc) {
        XPCThrower::Throw(NS_ERROR_UNEXPECTED, cx);
        return false;
    }

    JSObject* glob = JS_GetGlobalForScopeChain(cx);

    nsCOMPtr<nsIXPConnectJSObjectHolder> holder;
    jsval retval;
    rv = xpc->WrapNativeToJSVal(cx, glob, native, nullptr,
                                &NS_GET_IID(nsISupports),
                                true, &retval, nullptr);
    if (NS_FAILED(rv)) {
        XPCThrower::Throw(rv, cx);
        return false;
    }

    JS_SET_RVAL(cx, vp, retval);
    return true;
}

static JSFunctionSpec gGlobalFun[] = {
    JS_FS("dump",    Dump,   1,0),
    JS_FS("debug",   Debug,  1,0),
    JS_FS("atob",    Atob,   1,0),
    JS_FS("btoa",    Btoa,   1,0),
    JS_FS("File",    File,   1,JSFUN_CONSTRUCTOR),
    JS_FS_END
};

class JSCLContextHelper
{
public:
    JSCLContextHelper(mozJSComponentLoader* loader);
    ~JSCLContextHelper();

    void reportErrorAfterPop(char *buf);

    operator JSContext*() const {return mContext;}

private:
    JSContext* mContext;
    nsIThreadJSContextStack* mContextStack;
    char*      mBuf;

    // prevent copying and assignment
    JSCLContextHelper(const JSCLContextHelper &) MOZ_DELETE;
    const JSCLContextHelper& operator=(const JSCLContextHelper &) MOZ_DELETE;
};


class JSCLAutoErrorReporterSetter
{
public:
    JSCLAutoErrorReporterSetter(JSContext* cx, JSErrorReporter reporter)
        {mContext = cx; mOldReporter = JS_SetErrorReporter(cx, reporter);}
    ~JSCLAutoErrorReporterSetter()
        {JS_SetErrorReporter(mContext, mOldReporter);}
private:
    JSContext* mContext;
    JSErrorReporter mOldReporter;

    JSCLAutoErrorReporterSetter(const JSCLAutoErrorReporterSetter &) MOZ_DELETE;
    const JSCLAutoErrorReporterSetter& operator=(const JSCLAutoErrorReporterSetter &) MOZ_DELETE;
};

static nsresult
ReportOnCaller(JSContext *callerContext,
               const char *format, ...) {
    if (!callerContext) {
        return NS_ERROR_FAILURE;
    }

    va_list ap;
    va_start(ap, format);

    char *buf = JS_vsmprintf(format, ap);
    if (!buf) {
        return NS_ERROR_OUT_OF_MEMORY;
    }

    JS_ReportError(callerContext, buf);
    JS_smprintf_free(buf);

    return NS_OK;
}

static nsresult
ReportOnCaller(JSCLContextHelper &helper,
               const char *format, ...)
{
    va_list ap;
    va_start(ap, format);

    char *buf = JS_vsmprintf(format, ap);
    if (!buf) {
        return NS_ERROR_OUT_OF_MEMORY;
    }

    helper.reportErrorAfterPop(buf);

    return NS_OK;
}

mozJSComponentLoader::mozJSComponentLoader()
    : mRuntime(nullptr),
      mContext(nullptr),
      mInitialized(false),
      mReuseLoaderGlobal(false)
{
    NS_ASSERTION(!sSelf, "mozJSComponentLoader should be a singleton");

#ifdef PR_LOGGING
    if (!gJSCLLog) {
        gJSCLLog = PR_NewLogModule("JSComponentLoader");
    }
#endif

    sSelf = this;
}

mozJSComponentLoader::~mozJSComponentLoader()
{
    if (mInitialized) {
        NS_ERROR("'xpcom-shutdown-loaders' was not fired before cleaning up mozJSComponentLoader");
        UnloadModules();
    }

    sSelf = nullptr;
}

mozJSComponentLoader*
mozJSComponentLoader::sSelf;

NS_IMPL_ISUPPORTS3(mozJSComponentLoader,
                   mozilla::ModuleLoader,
                   xpcIJSModuleLoader,
                   nsIObserver)

nsresult
mozJSComponentLoader::ReallyInit()
{
    nsresult rv;

    mReuseLoaderGlobal = Preferences::GetBool("jsloader.reuseGlobal");

    /*
     * Get the JSRuntime from the runtime svc, if possible.
     * We keep a reference around, because it's a Bad Thing if the runtime
     * service gets shut down before we're done.  Bad!
     */

    mRuntimeService = do_GetService(kJSRuntimeServiceContractID, &rv);
    if (NS_FAILED(rv) ||
        NS_FAILED(rv = mRuntimeService->GetRuntime(&mRuntime)))
        return rv;

    mContextStack = do_GetService("@mozilla.org/js/xpc/ContextStack;1", &rv);
    if (NS_FAILED(rv))
        return rv;

    // Create our compilation context.
    mContext = JS_NewContext(mRuntime, 256);
    if (!mContext)
        return NS_ERROR_OUT_OF_MEMORY;

    if (Preferences::GetBool("javascript.options.xml.chrome")) {
        uint32_t options = JS_GetOptions(mContext);
        JS_SetOptions(mContext, options | JSOPTION_ALLOW_XML | JSOPTION_MOAR_XML);
    }

    // Always use the latest js version
    JS_SetVersion(mContext, JSVERSION_LATEST);

    nsCOMPtr<nsIScriptSecurityManager> secman =
        do_GetService(NS_SCRIPTSECURITYMANAGER_CONTRACTID);
    if (!secman)
        return NS_ERROR_FAILURE;

    rv = secman->GetSystemPrincipal(getter_AddRefs(mSystemPrincipal));
    if (NS_FAILED(rv) || !mSystemPrincipal)
        return NS_ERROR_FAILURE;

    mModules.Init(32);
    mImports.Init(32);
    mInProgressImports.Init(32);
    mThisObjects.Init(32);

    nsCOMPtr<nsIObserverService> obsSvc =
        do_GetService(kObserverServiceContractID, &rv);
    NS_ENSURE_SUCCESS(rv, rv);

    rv = obsSvc->AddObserver(this, "xpcom-shutdown-loaders", false);
    NS_ENSURE_SUCCESS(rv, rv);

    // Set up localized comparison and string conversion
    xpc_LocalizeContext(mContext);

#ifdef DEBUG_shaver_off
    fprintf(stderr, "mJCL: ReallyInit success!\n");
#endif
    mInitialized = true;

    return NS_OK;
}

const mozilla::Module*
mozJSComponentLoader::LoadModule(FileLocation &aFile)
{
    nsCOMPtr<nsIFile> file = aFile.GetBaseFile();

    nsCString spec;
    aFile.GetURIString(spec);

    nsCOMPtr<nsIURI> uri;
    nsresult rv = NS_NewURI(getter_AddRefs(uri), spec);
    if (NS_FAILED(rv))
        return NULL;

    if (!mInitialized) {
        rv = ReallyInit();
        if (NS_FAILED(rv))
            return NULL;
    }

    ModuleEntry* mod;
    if (mModules.Get(spec, &mod))
    return mod;

    nsAutoPtr<ModuleEntry> entry(new ModuleEntry);

    rv = ObjectForLocation(file, uri, &entry->obj,
                           &entry->location, nullptr);
    if (NS_FAILED(rv)) {
        return NULL;
    }

    nsCOMPtr<nsIXPConnect> xpc = do_GetService(kXPConnectServiceContractID,
                                               &rv);
    if (NS_FAILED(rv))
        return NULL;

    nsCOMPtr<nsIComponentManager> cm;
    rv = NS_GetComponentManager(getter_AddRefs(cm));
    if (NS_FAILED(rv))
        return NULL;

    JSCLContextHelper cx(this);
    JSAutoCompartment ac(cx, entry->obj);

    JSObject* cm_jsobj;
    nsCOMPtr<nsIXPConnectJSObjectHolder> cm_holder;
    rv = xpc->WrapNative(cx, entry->obj, cm,
                         NS_GET_IID(nsIComponentManager),
                         getter_AddRefs(cm_holder));

    if (NS_FAILED(rv)) {
#ifdef DEBUG_shaver
        fprintf(stderr, "WrapNative(%p,%p,nsIComponentManager) failed: %x\n",
                (void *)(JSContext*)cx, (void *)mCompMgr, rv);
#endif
        return NULL;
    }

    rv = cm_holder->GetJSObject(&cm_jsobj);
    if (NS_FAILED(rv)) {
#ifdef DEBUG_shaver
        fprintf(stderr, "GetJSObject of ComponentManager failed\n");
#endif
        return NULL;
    }

    JSObject* file_jsobj;
    nsCOMPtr<nsIXPConnectJSObjectHolder> file_holder;
    rv = xpc->WrapNative(cx, entry->obj, file,
                         NS_GET_IID(nsIFile),
                         getter_AddRefs(file_holder));

    if (NS_FAILED(rv)) {
        return NULL;
    }

    rv = file_holder->GetJSObject(&file_jsobj);
    if (NS_FAILED(rv)) {
        return NULL;
    }

    JSCLAutoErrorReporterSetter aers(cx, mozJSLoaderErrorReporter);

    jsval NSGetFactory_val;

    if (!JS_GetProperty(cx, entry->obj, "NSGetFactory", &NSGetFactory_val) ||
        JSVAL_IS_VOID(NSGetFactory_val)) {
        return NULL;
    }

    if (JS_TypeOfValue(cx, NSGetFactory_val) != JSTYPE_FUNCTION) {
        nsAutoCString spec;
        uri->GetSpec(spec);
        JS_ReportError(cx, "%s has NSGetFactory property that is not a function",
                       spec.get());
        return NULL;
    }

    JSObject *jsGetFactoryObj;
    if (!JS_ValueToObject(cx, NSGetFactory_val, &jsGetFactoryObj) ||
        !jsGetFactoryObj) {
        /* XXX report error properly */
        return NULL;
    }

    rv = xpc->WrapJS(cx, jsGetFactoryObj,
                     NS_GET_IID(xpcIJSGetFactory), getter_AddRefs(entry->getfactoryobj));
    if (NS_FAILED(rv)) {
        /* XXX report error properly */
#ifdef DEBUG
        fprintf(stderr, "mJCL: couldn't get nsIModule from jsval\n");
#endif
        return NULL;
    }

    // Cache this module for later
    mModules.Put(spec, entry);

    // Set the location information for the new global, so that tools like
    // about:memory may use that information
    if (!mReuseLoaderGlobal) {
        xpc::SetLocationForGlobal(entry->obj, spec);
    }

    // The hash owns the ModuleEntry now, forget about it
    return entry.forget();
}

nsresult
mozJSComponentLoader::FindTargetObject(JSContext* aCx,
                                       JSObject** aTargetObject)
{
    JSObject* targetObject = nullptr;
    *aTargetObject = nullptr;

    if (mReuseLoaderGlobal) {
        JSScript* script =
            js::GetOutermostEnclosingFunctionOfScriptedCaller(aCx);
        if (script) {
            targetObject = mThisObjects.Get(script);
        }
    }

    // The above could fail, even if mReuseLoaderGlobal, if the scripted
    // caller is not a component/JSM (it could be a DOM scope, for
    // instance).
    if (!targetObject) {
        // Our targetObject is the caller's global object. Let's get it.
        nsresult rv;
        nsCOMPtr<nsIXPConnect> xpc =
            do_GetService(kXPConnectServiceContractID, &rv);
        NS_ENSURE_SUCCESS(rv, rv);

        nsAXPCNativeCallContext *cc = nullptr;
        rv = xpc->GetCurrentNativeCallContext(&cc);
        NS_ENSURE_SUCCESS(rv, rv);

        nsCOMPtr<nsIXPConnectWrappedNative> wn;
        rv = cc->GetCalleeWrapper(getter_AddRefs(wn));
        NS_ENSURE_SUCCESS(rv, rv);

        wn->GetJSObject(&targetObject);
        if (!targetObject) {
            NS_ERROR("null calling object");
            return NS_ERROR_FAILURE;
        }

        targetObject = JS_GetGlobalForObject(aCx, targetObject);
    }

    *aTargetObject = targetObject;
    return NS_OK;
}

void
mozJSComponentLoader::NoteSubScript(JSScript* aScript, JSObject* aThisObject)
{
  if (!mInitialized && NS_FAILED(ReallyInit())) {
      MOZ_NOT_REACHED();
  }

  mThisObjects.Put(aScript, aThisObject);
}

// Some stack based classes for cleaning up on early return
#ifdef HAVE_PR_MEMMAP
class FileAutoCloser
{
 public:
    explicit FileAutoCloser(PRFileDesc *file) : mFile(file) {}
    ~FileAutoCloser() { PR_Close(mFile); }
 private:
    PRFileDesc *mFile;
};

class FileMapAutoCloser
{
 public:
    explicit FileMapAutoCloser(PRFileMap *map) : mMap(map) {}
    ~FileMapAutoCloser() { PR_CloseFileMap(mMap); }
 private:
    PRFileMap *mMap;
};
#else
class ANSIFileAutoCloser
{
 public:
    explicit ANSIFileAutoCloser(FILE *file) : mFile(file) {}
    ~ANSIFileAutoCloser() { fclose(mFile); }
 private:
    FILE *mFile;
};
#endif

JSObject*
mozJSComponentLoader::PrepareObjectForLocation(JSCLContextHelper& aCx,
                                               nsIFile *aComponentFile,
                                               nsIURI *aURI,
                                               bool aReuseLoaderGlobal,
                                               bool *aRealFile)
{
    nsCOMPtr<nsIXPConnectJSObjectHolder> holder;
    if (aReuseLoaderGlobal) {
        holder = mLoaderGlobal;
    }

    nsresult rv = NS_OK;
    nsCOMPtr<nsIXPConnect> xpc =
        do_GetService(kXPConnectServiceContractID, &rv);
    NS_ENSURE_SUCCESS(rv, nullptr);

    if (!mLoaderGlobal) {
        nsCOMPtr<nsIXPCScriptable> backstagePass;
        rv = mRuntimeService->GetBackstagePass(getter_AddRefs(backstagePass));
        NS_ENSURE_SUCCESS(rv, nullptr);

        rv = xpc->InitClassesWithNewWrappedGlobal(aCx, backstagePass,
                                                  mSystemPrincipal,
                                                  0,
                                                  getter_AddRefs(holder));
        NS_ENSURE_SUCCESS(rv, nullptr);

        JSObject *global;
        rv = holder->GetJSObject(&global);
        NS_ENSURE_SUCCESS(rv, nullptr);

        JSAutoCompartment ac(aCx, global);
        if (!JS_DefineFunctions(aCx, global, gGlobalFun) ||
            !JS_DefineProfilingFunctions(aCx, global)) {
            return nullptr;
        }

        if (aReuseLoaderGlobal) {
            mLoaderGlobal = holder;
        }
    }

    JSObject* obj;
    rv = holder->GetJSObject(&obj);
    NS_ENSURE_SUCCESS(rv, nullptr);

    JSAutoCompartment ac(aCx, obj);

    if (aReuseLoaderGlobal) {
        // If we're reusing the loader global, we don't actually use the
        // global, but rather we use a different object as the 'this' object.
        JSObject* newObj = JS_NewObject(aCx, &kFakeBackstagePassJSClass,
                                        nullptr, nullptr);
        NS_ENSURE_TRUE(newObj, nullptr);

        obj = newObj;
    }

    *aRealFile = false;

    // need to be extra careful checking for URIs pointing to files
    // EnsureFile may not always get called, especially on resource URIs
    // so we need to call GetFile to make sure this is a valid file
    nsCOMPtr<nsIFileURL> fileURL = do_QueryInterface(aURI, &rv);
    nsCOMPtr<nsIFile> testFile;
    if (NS_SUCCEEDED(rv)) {
        fileURL->GetFile(getter_AddRefs(testFile));
    }

    if (testFile) {
        *aRealFile = true;

        nsCOMPtr<nsIXPConnectJSObjectHolder> locationHolder;
        rv = xpc->WrapNative(aCx, obj, aComponentFile,
                             NS_GET_IID(nsIFile),
                             getter_AddRefs(locationHolder));
        NS_ENSURE_SUCCESS(rv, nullptr);

        JSObject *locationObj;
        rv = locationHolder->GetJSObject(&locationObj);
        NS_ENSURE_SUCCESS(rv, nullptr);

        if (!JS_DefineProperty(aCx, obj, "__LOCATION__",
                               OBJECT_TO_JSVAL(locationObj),
                               nullptr, nullptr, 0)) {
            return nullptr;
        }
    }

    nsAutoCString nativePath;
    rv = aURI->GetSpec(nativePath);
    NS_ENSURE_SUCCESS(rv, nullptr);

    // Expose the URI from which the script was imported through a special
    // variable that we insert into the JSM.
    JSString *exposedUri = JS_NewStringCopyN(aCx, nativePath.get(),
                                             nativePath.Length());
    if (!JS_DefineProperty(aCx, obj, "__URI__",
                           STRING_TO_JSVAL(exposedUri), nullptr, nullptr, 0)) {
        return nullptr;
    }

    return obj;
}

nsresult
mozJSComponentLoader::ObjectForLocation(nsIFile *aComponentFile,
                                        nsIURI *aURI,
                                        JSObject **aObject,
                                        char **aLocation,
                                        jsval *exception)
{
    JSCLContextHelper cx(this);

    JS_AbortIfWrongThread(JS_GetRuntime(cx));

    JSCLAutoErrorReporterSetter aers(cx, mozJSLoaderErrorReporter);

    bool realFile = false;
    JSObject *obj = PrepareObjectForLocation(cx, aComponentFile, aURI,
                                             mReuseLoaderGlobal, &realFile);
    NS_ENSURE_TRUE(obj, NS_ERROR_FAILURE);

    JSAutoCompartment ac(cx, obj);

    JSScript *script = nullptr;
    JSFunction *function = nullptr;

    nsAutoCString nativePath;
    nsresult rv = aURI->GetSpec(nativePath);
    NS_ENSURE_SUCCESS(rv, rv);

    // Before compiling the script, first check to see if we have it in
    // the startupcache.  Note: as a rule, startupcache errors are not fatal
    // to loading the script, since we can always slow-load.

    bool writeToCache = false;
    StartupCache* cache = StartupCache::GetSingleton();

    nsAutoCString cachePath(kJSCachePrefix);
    rv = PathifyURI(aURI, cachePath);
    NS_ENSURE_SUCCESS(rv, rv);

    if (cache) {
        if (!mReuseLoaderGlobal) {
            rv = ReadCachedScript(cache, cachePath, cx, mSystemPrincipal,
                                  &script);
        } else {
            rv = ReadCachedFunction(cache, cachePath, cx, mSystemPrincipal,
                                    &function);
        }

        if (NS_SUCCEEDED(rv)) {
            LOG(("Successfully loaded %s from startupcache\n", nativePath.get()));
        } else {
            // This is ok, it just means the script is not yet in the
            // cache. Could mean that the cache was corrupted and got removed,
            // but either way we're going to write this out.
            writeToCache = true;
        }
    }

    if (!script && !function) {
        // The script wasn't in the cache , so compile it now.
        LOG(("Slow loading %s\n", nativePath.get()));

        // If |exception| is non-null, then our caller wants us to propagate
        // any exceptions out to our caller. Ensure that the engine doesn't
        // eagerly report the exception.
        uint32_t oldopts = JS_GetOptions(cx);
        if (exception)
            JS_SetOptions(cx, oldopts | JSOPTION_DONT_REPORT_UNCAUGHT);
        JS::CompileOptions options(cx);
        options.setPrincipals(nsJSPrincipals::get(mSystemPrincipal))
               .setNoScriptRval(mReuseLoaderGlobal ? false : true)
               .setVersion(JSVERSION_LATEST)
               .setFileAndLine(nativePath.get(), 1)
               .setSourcePolicy(mReuseLoaderGlobal ?
                                JS::CompileOptions::NO_SOURCE :
                                JS::CompileOptions::LAZY_SOURCE);
        js::RootedObject rootedObject(cx, obj);

        if (realFile) {
#ifdef HAVE_PR_MEMMAP
            int64_t fileSize;
            rv = aComponentFile->GetFileSize(&fileSize);
            if (NS_FAILED(rv)) {
                JS_SetOptions(cx, oldopts);
                return rv;
            }

            int64_t maxSize = UINT32_MAX;
            if (fileSize > maxSize) {
                NS_ERROR("file too large");
                JS_SetOptions(cx, oldopts);
                return NS_ERROR_FAILURE;
            }

            PRFileDesc *fileHandle;
            rv = aComponentFile->OpenNSPRFileDesc(PR_RDONLY, 0, &fileHandle);
            if (NS_FAILED(rv)) {
                JS_SetOptions(cx, oldopts);
                return NS_ERROR_FILE_NOT_FOUND;
            }

            // Make sure the file is closed, no matter how we return.
            FileAutoCloser fileCloser(fileHandle);

            PRFileMap *map = PR_CreateFileMap(fileHandle, fileSize,
                                              PR_PROT_READONLY);
            if (!map) {
                NS_ERROR("Failed to create file map");
                JS_SetOptions(cx, oldopts);
                return NS_ERROR_FAILURE;
            }

            // Make sure the file map is closed, no matter how we return.
            FileMapAutoCloser mapCloser(map);

            uint32_t fileSize32 = fileSize;

            char *buf = static_cast<char*>(PR_MemMap(map, 0, fileSize32));
            if (!buf) {
                NS_WARNING("Failed to map file");
                JS_SetOptions(cx, oldopts);
                return NS_ERROR_FAILURE;
            }

            if (!mReuseLoaderGlobal) {
                script = JS::Compile(cx, rootedObject, options, buf,
                                     fileSize32);
            } else {
                function = JS::CompileFunction(cx, rootedObject, options,
                                               nullptr, 0, nullptr,
                                               buf, fileSize32);
            }

            PR_MemUnmap(buf, fileSize32);

#else  /* HAVE_PR_MEMMAP */

            /**
             * No memmap implementation, so fall back to 
             * reading in the file
             */

            FILE *fileHandle;
            rv = aComponentFile->OpenANSIFileDesc("r", &fileHandle);
            if (NS_FAILED(rv)) {
                JS_SetOptions(cx, oldopts);
                return NS_ERROR_FILE_NOT_FOUND;
            }

            // Ensure file fclose
            ANSIFileAutoCloser fileCloser(fileHandle);

            int64_t len;
            rv = aComponentFile->GetFileSize(&len);
            if (NS_FAILED(rv) || len < 0) {
                NS_WARNING("Failed to get file size");
                JS_SetOptions(cx, oldopts);
                return NS_ERROR_FAILURE;
            }

            char *buf = (char *) malloc(len * sizeof(char));
            if (!buf) {
                JS_SetOptions(cx, oldopts);
                return NS_ERROR_FAILURE;
            }

            size_t rlen = fread(buf, 1, len, fileHandle);
            if (rlen != (uint64_t)len) {
                free(buf);
                JS_SetOptions(cx, oldopts);
                NS_WARNING("Failed to read file");
                return NS_ERROR_FAILURE;
            }

            if (!mReuseLoaderGlobal) {
                script = JS::Compile(cx, rootedObject, options, buf,
                                     fileSize32);
            } else {
                function = JS::CompileFunction(cx, rootedObject, options,
                                               nullptr, 0, nullptr,
                                               buf, fileSize32);
            }

            free(buf);

#endif /* HAVE_PR_MEMMAP */
        } else {
            nsCOMPtr<nsIIOService> ioService = do_GetIOService(&rv);
            NS_ENSURE_SUCCESS(rv, rv);

            nsCOMPtr<nsIChannel> scriptChannel;
            rv = ioService->NewChannelFromURI(aURI, getter_AddRefs(scriptChannel));
            NS_ENSURE_SUCCESS(rv, rv);

            nsCOMPtr<nsIInputStream> scriptStream;
            rv = scriptChannel->Open(getter_AddRefs(scriptStream));
            NS_ENSURE_SUCCESS(rv, rv);

            uint64_t len64;
            uint32_t bytesRead;

            rv = scriptStream->Available(&len64);
            NS_ENSURE_SUCCESS(rv, rv);
            NS_ENSURE_TRUE(len64 < UINT32_MAX, NS_ERROR_FILE_TOO_BIG);
            if (!len64)
                return NS_ERROR_FAILURE;
            uint32_t len = (uint32_t)len64;

            /* malloc an internal buf the size of the file */
            nsAutoArrayPtr<char> buf(new char[len + 1]);
            if (!buf)
                return NS_ERROR_OUT_OF_MEMORY;

            /* read the file in one swoop */
            rv = scriptStream->Read(buf, len, &bytesRead);
            if (bytesRead != len)
                return NS_BASE_STREAM_OSERROR;

            buf[len] = '\0';

            if (!mReuseLoaderGlobal) {
                script = JS::Compile(cx, rootedObject, options, buf,
                                     bytesRead);
            } else {
                function = JS::CompileFunction(cx, rootedObject, options,
                                               nullptr, 0, nullptr,
                                               buf, bytesRead);
            }
        }
        // Propagate the exception, if one exists. Also, don't leave the stale
        // exception on this context.
        JS_SetOptions(cx, oldopts);
        if (!script && !function && exception) {
            JS_GetPendingException(cx, exception);
            JS_ClearPendingException(cx);
        }
    }

    if (!script && !function) {
        return NS_ERROR_FAILURE;
    }

    if (writeToCache) {
        // We successfully compiled the script, so cache it.
        if (script) {
            rv = WriteCachedScript(cache, cachePath, cx, mSystemPrincipal,
                                   script);
        } else {
            rv = WriteCachedFunction(cache, cachePath, cx, mSystemPrincipal,
                                     function);
        }

        // Don't treat failure to write as fatal, since we might be working
        // with a read-only cache.
        if (NS_SUCCEEDED(rv)) {
            LOG(("Successfully wrote to cache\n"));
        } else {
            LOG(("Failed to write to cache\n"));
        }
    }

    // Assign aObject here so that it's available to recursive imports.
    // See bug 384168.
    *aObject = obj;

    JSScript* tableScript = script;
    if (!tableScript) {
        tableScript = JS_GetFunctionScript(cx, function);
        MOZ_ASSERT(tableScript);
    }

    mThisObjects.Put(tableScript, obj);

    uint32_t oldopts = JS_GetOptions(cx);
    JS_SetOptions(cx, oldopts |
                  (exception ? JSOPTION_DONT_REPORT_UNCAUGHT : 0));
    bool ok = false;
    if (script) {
        ok = JS_ExecuteScriptVersion(cx, obj, script, NULL, JSVERSION_LATEST);
    } else {
        jsval rval;
        ok = JS_CallFunction(cx, obj, function, 0, nullptr, &rval);
    }
    JS_SetOptions(cx, oldopts);

    if (!ok) {
#ifdef DEBUG_shaver_off
        fprintf(stderr, "mJCL: failed to execute %s\n", nativePath.get());
#endif
        if (exception) {
            JS_GetPendingException(cx, exception);
            JS_ClearPendingException(cx);
        }
        *aObject = nullptr;
        return NS_ERROR_FAILURE;
    }

    /* Freed when we remove from the table. */
    *aLocation = ToNewCString(nativePath);
    if (!*aLocation) {
        *aObject = nullptr;
        return NS_ERROR_OUT_OF_MEMORY;
    }

    JS_AddNamedObjectRoot(cx, aObject, *aLocation);
    return NS_OK;
}

/* static */ PLDHashOperator
mozJSComponentLoader::ClearModules(const nsACString& key, ModuleEntry*& entry, void* cx)
{
    entry->Clear();
    return PL_DHASH_REMOVE;
}

void
mozJSComponentLoader::UnloadModules()
{
    mInitialized = false;

    if (mLoaderGlobal) {
        MOZ_ASSERT(mReuseLoaderGlobal, "How did this happen?");

        JSObject* global;
        if (NS_SUCCEEDED(mLoaderGlobal->GetJSObject(&global))) {
            JSAutoRequest ar(mContext);
            JS_SetAllNonReservedSlotsToUndefined(mContext, global);
        } else {
            NS_WARNING("Going to leak!");
        }

        mLoaderGlobal = nullptr;
    }

    mInProgressImports.Clear();
    mImports.Clear();
    mThisObjects.Clear();

    mModules.Enumerate(ClearModules, NULL);

    // Destroying our context will force a GC.
    JS_DestroyContext(mContext);
    mContext = nullptr;

    mRuntimeService = nullptr;
    mContextStack = nullptr;
#ifdef DEBUG_shaver_off
    fprintf(stderr, "mJCL: UnloadAll(%d)\n", aWhen);
#endif
}

NS_IMETHODIMP
mozJSComponentLoader::Import(const nsACString& registryLocation,
                             const JS::Value& targetVal_,
                             JSContext* cx,
                             uint8_t optionalArgc,
                             JS::Value* retval)
{
    JSAutoRequest ar(cx);

    JS::Value targetVal = targetVal_;
    JSObject *targetObject = NULL;

    MOZ_ASSERT(nsContentUtils::IsCallerChrome());
    if (optionalArgc) {
        // The caller passed in the optional second argument. Get it.
        if (targetVal.isObject()) {
            // If we're passing in something like a content DOM window, chances
            // are the caller expects the properties to end up on the object
            // proper and not on the Xray holder. This is dubious, but can be used
            // during testing. Given that dumb callers can already leak JSMs into
            // content by passing a raw content JS object (where Xrays aren't
            // possible), we aim for consistency here. Waive xray.
            if (WrapperFactory::IsXrayWrapper(&targetVal.toObject()) &&
                !WrapperFactory::WaiveXrayAndWrap(cx, &targetVal))
            {
                return NS_ERROR_FAILURE;
            }
            targetObject = &targetVal.toObject();
        } else if (!targetVal.isNull()) {
            // If targetVal isNull(), we actually want to leave targetObject null.
            // Not doing so breaks |make package|.
            return ReportOnCaller(cx, ERROR_SCOPE_OBJ,
                                  PromiseFlatCString(registryLocation).get());
        }
    } else {
        nsresult rv = FindTargetObject(cx, &targetObject);
        NS_ENSURE_SUCCESS(rv, rv);
    }

    Maybe<JSAutoCompartment> ac;
    if (targetObject) {
        ac.construct(cx, targetObject);
    }

    JSObject *globalObj = nullptr;
    nsresult rv = ImportInto(registryLocation, targetObject, cx, &globalObj);

    if (globalObj && !JS_WrapObject(cx, &globalObj)) {
        NS_ERROR("can't wrap return value");
        return NS_ERROR_FAILURE;
    }

    *retval = OBJECT_TO_JSVAL(globalObj);

    return rv;
}

/* [noscript] JSObjectPtr importInto(in AUTF8String registryLocation,
                                     in JSObjectPtr targetObj); */
NS_IMETHODIMP
mozJSComponentLoader::ImportInto(const nsACString & aLocation,
                                 JSObject * targetObj,
                                 nsAXPCNativeCallContext * cc,
                                 JSObject * *_retval)
{
    JSContext *callercx;
    nsresult rv = cc->GetJSContext(&callercx);
    NS_ENSURE_SUCCESS(rv, rv);
    return ImportInto(aLocation, targetObj, callercx, _retval);
}

nsresult
mozJSComponentLoader::ImportInto(const nsACString & aLocation,
                                 JSObject * targetObj,
                                 JSContext * callercx,
                                 JSObject * *_retval)
{
    nsresult rv;
    *_retval = nullptr;

    if (!mInitialized) {
        rv = ReallyInit();
        NS_ENSURE_SUCCESS(rv, rv);
    }

    nsCOMPtr<nsIIOService> ioService = do_GetIOService(&rv);
    NS_ENSURE_SUCCESS(rv, rv);

    // Get the URI.
    nsCOMPtr<nsIURI> resURI;
    rv = ioService->NewURI(aLocation, nullptr, nullptr, getter_AddRefs(resURI));
    NS_ENSURE_SUCCESS(rv, rv);

    // figure out the resolved URI
    nsCOMPtr<nsIChannel> scriptChannel;
    rv = ioService->NewChannelFromURI(resURI, getter_AddRefs(scriptChannel));
    NS_ENSURE_SUCCESS(rv, NS_ERROR_INVALID_ARG);

    nsCOMPtr<nsIURI> resolvedURI;
    rv = scriptChannel->GetURI(getter_AddRefs(resolvedURI));
    NS_ENSURE_SUCCESS(rv, rv);

    // get the JAR if there is one
    nsCOMPtr<nsIJARURI> jarURI;
    jarURI = do_QueryInterface(resolvedURI, &rv);
    nsCOMPtr<nsIFileURL> baseFileURL;
    if (NS_SUCCEEDED(rv)) {
        nsCOMPtr<nsIURI> baseURI;
        while (jarURI) {
            jarURI->GetJARFile(getter_AddRefs(baseURI));
            jarURI = do_QueryInterface(baseURI, &rv);
        }
        baseFileURL = do_QueryInterface(baseURI, &rv);
        NS_ENSURE_SUCCESS(rv, rv);
    } else {
        baseFileURL = do_QueryInterface(resolvedURI, &rv);
        NS_ENSURE_SUCCESS(rv, rv);
    }

    nsCOMPtr<nsIFile> sourceFile;
    rv = baseFileURL->GetFile(getter_AddRefs(sourceFile));
    NS_ENSURE_SUCCESS(rv, rv);

    nsCOMPtr<nsIFile> sourceLocalFile;
    sourceLocalFile = do_QueryInterface(sourceFile, &rv);
    NS_ENSURE_SUCCESS(rv, rv);

    nsAutoCString key;
    rv = resolvedURI->GetSpec(key);
    NS_ENSURE_SUCCESS(rv, rv);

    ModuleEntry* mod;
    nsAutoPtr<ModuleEntry> newEntry;
    if (!mImports.Get(key, &mod) && !mInProgressImports.Get(key, &mod)) {
        newEntry = new ModuleEntry;
        if (!newEntry)
            return NS_ERROR_OUT_OF_MEMORY;
        mInProgressImports.Put(key, newEntry);

        JS::Anchor<jsval> exception(JSVAL_VOID);
        rv = ObjectForLocation(sourceLocalFile, resURI, &newEntry->obj,
                               &newEntry->location, &exception.get());

        mInProgressImports.Remove(key);

        if (NS_FAILED(rv)) {
            *_retval = nullptr;

            if (!JSVAL_IS_VOID(exception.get())) {
                // An exception was thrown during compilation. Propagate it
                // out to our caller so they can report it.
                if (!JS_WrapValue(callercx, &exception.get()))
                    return NS_ERROR_OUT_OF_MEMORY;
                JS_SetPendingException(callercx, exception.get());
                return NS_OK;
            }

            // Something failed, but we don't know what it is, guess.
            return NS_ERROR_FILE_NOT_FOUND;
        }

        // Set the location information for the new global, so that tools like
        // about:memory may use that information
        if (!mReuseLoaderGlobal) {
            xpc::SetLocationForGlobal(newEntry->obj, aLocation);
        }

        mod = newEntry;
    }

    NS_ASSERTION(mod->obj, "Import table contains entry with no object");
    *_retval = mod->obj;

    if (targetObj) {
        JSCLContextHelper cxhelper(this);
        JSAutoCompartment ac(mContext, mod->obj);

        JS::Value symbols;
        if (!JS_GetProperty(mContext, mod->obj,
                            "EXPORTED_SYMBOLS", &symbols)) {
            return ReportOnCaller(cxhelper, ERROR_NOT_PRESENT,
                                  PromiseFlatCString(aLocation).get());
        }

        if (!symbols.isObject() ||
            !JS_IsArrayObject(mContext, &symbols.toObject())) {
            return ReportOnCaller(cxhelper, ERROR_NOT_AN_ARRAY,
                                  PromiseFlatCString(aLocation).get());
        }

        JSObject *symbolsObj = &symbols.toObject();

        // Iterate over symbols array, installing symbols on targetObj:

        uint32_t symbolCount = 0;
        if (!JS_GetArrayLength(mContext, symbolsObj, &symbolCount)) {
            return ReportOnCaller(cxhelper, ERROR_GETTING_ARRAY_LENGTH,
                                  PromiseFlatCString(aLocation).get());
        }

#ifdef DEBUG
        nsAutoCString logBuffer;
#endif

        for (uint32_t i = 0; i < symbolCount; ++i) {
            jsval val;
            jsid symbolId;

            if (!JS_GetElement(mContext, symbolsObj, i, &val) ||
                !JSVAL_IS_STRING(val) ||
                !JS_ValueToId(mContext, val, &symbolId)) {
                return ReportOnCaller(cxhelper, ERROR_ARRAY_ELEMENT,
                                      PromiseFlatCString(aLocation).get(), i);
            }

            if (!JS_GetPropertyById(mContext, mod->obj, symbolId, &val)) {
                JSAutoByteString bytes(mContext, JSID_TO_STRING(symbolId));
                if (!bytes)
                    return NS_ERROR_FAILURE;
                return ReportOnCaller(cxhelper, ERROR_GETTING_SYMBOL,
                                      PromiseFlatCString(aLocation).get(),
                                      bytes.ptr());
            }

            JSAutoCompartment target_ac(mContext, targetObj);

            if (!JS_WrapValue(mContext, &val) ||
                !JS_SetPropertyById(mContext, targetObj, symbolId, &val)) {
                JSAutoByteString bytes(mContext, JSID_TO_STRING(symbolId));
                if (!bytes)
                    return NS_ERROR_FAILURE;
                return ReportOnCaller(cxhelper, ERROR_SETTING_SYMBOL,
                                      PromiseFlatCString(aLocation).get(),
                                      bytes.ptr());
            }
#ifdef DEBUG
            if (i == 0) {
                logBuffer.AssignLiteral("Installing symbols [ ");
            }
            JSAutoByteString bytes(mContext, JSID_TO_STRING(symbolId));
            if (!!bytes)
                logBuffer.Append(bytes.ptr());
            logBuffer.AppendLiteral(" ");
            if (i == symbolCount - 1) {
                LOG(("%s] from %s\n", logBuffer.get(),
                     PromiseFlatCString(aLocation).get()));
            }
#endif
        }
    }

    // Cache this module for later
    if (newEntry) {
        mImports.Put(key, newEntry);
        newEntry.forget();
    }

    return NS_OK;
}

NS_IMETHODIMP
mozJSComponentLoader::Unload(const nsACString & aLocation)
{
    nsresult rv;

    if (!mInitialized) {
        return NS_OK;
    }

    nsCOMPtr<nsIIOService> ioService = do_GetIOService(&rv);
    NS_ENSURE_SUCCESS(rv, rv);

    // Get the URI.
    nsCOMPtr<nsIURI> resURI;
    rv = ioService->NewURI(aLocation, nullptr, nullptr, getter_AddRefs(resURI));
    NS_ENSURE_SUCCESS(rv, rv);

    // figure out the resolved URI
    nsCOMPtr<nsIChannel> scriptChannel;
    rv = ioService->NewChannelFromURI(resURI, getter_AddRefs(scriptChannel));
    NS_ENSURE_SUCCESS(rv, NS_ERROR_INVALID_ARG);

    nsCOMPtr<nsIURI> resolvedURI;
    rv = scriptChannel->GetURI(getter_AddRefs(resolvedURI));
    NS_ENSURE_SUCCESS(rv, rv);

    nsAutoCString key;
    rv = resolvedURI->GetSpec(key);
    NS_ENSURE_SUCCESS(rv, rv);

    ModuleEntry* mod;
    if (mImports.Get(key, &mod)) {
        mImports.Remove(key);
    }

    return NS_OK;
}

NS_IMETHODIMP
mozJSComponentLoader::Observe(nsISupports *subject, const char *topic,
                              const PRUnichar *data)
{
    if (!strcmp(topic, "xpcom-shutdown-loaders")) {
        UnloadModules();
    } else {
        NS_ERROR("Unexpected observer topic.");
    }

    return NS_OK;
}

/* static */ already_AddRefed<nsIFactory>
mozJSComponentLoader::ModuleEntry::GetFactory(const mozilla::Module& module,
                                              const mozilla::Module::CIDEntry& entry)
{
    const ModuleEntry& self = static_cast<const ModuleEntry&>(module);
    NS_ASSERTION(self.getfactoryobj, "Handing out an uninitialized module?");

    nsCOMPtr<nsIFactory> f;
    nsresult rv = self.getfactoryobj->Get(*entry.cid, getter_AddRefs(f));
    if (NS_FAILED(rv))
        return NULL;

    return f.forget();
}

//----------------------------------------------------------------------

JSCLContextHelper::JSCLContextHelper(mozJSComponentLoader *loader)
    : mContext(loader->mContext),
      mContextStack(loader->mContextStack),
      mBuf(nullptr)
{
    mContextStack->Push(mContext);
    JS_BeginRequest(mContext);
}

JSCLContextHelper::~JSCLContextHelper()
{
    if (mContextStack) {
        JS_EndRequest(mContext);

        mContextStack->Pop(nullptr);

        JSContext* cx = nullptr;
        mContextStack->Peek(&cx);

        mContextStack = nullptr;

        if (cx && mBuf) {
            JS_ReportError(cx, mBuf);
        }
    }

    if (mBuf) {
        JS_smprintf_free(mBuf);
    }
}

void
JSCLContextHelper::reportErrorAfterPop(char *buf)
{
    NS_ASSERTION(!mBuf, "Already called reportErrorAfterPop");
    mBuf = buf;
}
