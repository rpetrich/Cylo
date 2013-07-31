#include <CoreFoundation/CoreFoundation.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <dlfcn.h>
#include <unistd.h>
#include <objc/runtime.h>

#include <JavaScriptCore/JSContextRef.h>
#include <JavaScriptCore/JSStringRef.h>
#include <JavaScriptCore/JSStringRefCF.h>
#include <JavaScriptCore/JSValueRef.h>
#include <apr-1/apr_pools.h>
#include <cycript.h>

#include "Log.hpp"

#define ForSaurik 0

#define Scripts_ "/Library/Cylo/Scripts"

extern "C" char ***_NSGetArgv(void);

__attribute__((constructor))
static void constructor(void)
{
	char *argv0(**_NSGetArgv());
	char *slash(strrchr(argv0, '/'));
	slash = slash == NULL ? argv0 : slash + 1;

	Class (*NSClassFromString)(CFStringRef) = reinterpret_cast<Class (*)(CFStringRef)>(dlsym(RTLD_DEFAULT, "NSClassFromString"));

	bool safe(false);

	CFURLRef scriptsURL(CFURLCreateFromFileSystemRepresentation(kCFAllocatorDefault, reinterpret_cast<const UInt8 *>(Scripts_), sizeof(Scripts_) - 1, TRUE));

	CFBundleRef folder(CFBundleCreate(kCFAllocatorDefault, scriptsURL));
	CFRelease(scriptsURL);

	if (folder == NULL)
		return;

	CFArrayRef jsFiles(CFBundleCopyResourceURLsOfType(folder, CFSTR("js"), NULL));
	CFArrayRef cyFiles(CFBundleCopyResourceURLsOfType(folder, CFSTR("cy"), NULL));
	CFRelease(folder);

	CFMutableArrayRef scripts = CFArrayCreateMutableCopy(kCFAllocatorDefault, 0, jsFiles);
	CFRelease(jsFiles);
	CFArrayAppendArray(scripts, cyFiles, CFRangeMake(0, CFArrayGetCount(cyFiles)));
	CFRelease(cyFiles);

	for (CFIndex i(0), count(CFArrayGetCount(scripts)); i != count; ++i) {
		CFURLRef script(reinterpret_cast<CFURLRef>(CFArrayGetValueAtIndex(scripts, i)));

		char path[PATH_MAX];
		CFURLGetFileSystemRepresentation(script, TRUE, reinterpret_cast<UInt8 *>(path), sizeof(path));
		size_t length(strlen(path));
		memcpy(path + length - 2, "plist", 6);

		CFURLRef plist(CFURLCreateFromFileSystemRepresentation(kCFAllocatorDefault, reinterpret_cast<UInt8 *>(path), length, FALSE));

		CFDataRef data;
		if (!CFURLCreateDataAndPropertiesFromResource(kCFAllocatorDefault, plist, &data, NULL, NULL, NULL))
			data = NULL;
		CFRelease(plist);

		CFDictionaryRef meta(NULL);
		if (data != NULL) {
			CFStringRef error(NULL);
			meta = reinterpret_cast<CFDictionaryRef>(CFPropertyListCreateFromXMLData(kCFAllocatorDefault, data, kCFPropertyListImmutable, &error));
			CFRelease(data);

			if (meta == NULL && error != NULL) {
				MSLog(MSLogLevelError, "Cylo:Error: Corrupt PropertyList: %@", script);
				continue;
			}
		}

		// XXX: eventually this should become false: scripts must have a filter
		bool load(!safe);

		if (meta != NULL) {
			if (CFDictionaryRef filter = reinterpret_cast<CFDictionaryRef>(CFDictionaryGetValue(meta, CFSTR("Filter")))) {
				load = true;

				int value(0);
				if (CFNumberRef flags = reinterpret_cast<CFNumberRef>(CFDictionaryGetValue(filter, CFSTR("Flags")))) {
					if (CFGetTypeID(flags) != CFNumberGetTypeID() || !CFNumberGetValue(flags, kCFNumberIntType, &value)) {
						MSLog(MSLogLevelError, "Cylo:Error: Unable to Read Flags: %@", flags);
						load = false;
						goto release;
					}
				}

				#define MSFlagWhenSafe  (1 << 0)
				#define MSFlagNotNoSafe (1 << 1)

				if ((value & MSFlagWhenSafe) == 0 && safe) {
					load = false;
					goto release;
				}

				if ((value & MSFlagNotNoSafe) != 0 && !safe) {
					load = false;
					goto release;
				}

				if (CFArrayRef version = reinterpret_cast<CFArrayRef>(CFDictionaryGetValue(filter, CFSTR("CoreFoundationVersion")))) {
					load = false;

					if (CFIndex count = CFArrayGetCount(version)) {
						if (count > 2) {
							MSLog(MSLogLevelError, "Cylo:Error: Invalid CoreFoundationVersion: %@", version);
							goto release;
						}

						CFNumberRef number;
						double value;

						number = reinterpret_cast<CFNumberRef>(CFArrayGetValueAtIndex(version, 0));

						if (CFGetTypeID(number) != CFNumberGetTypeID() || !CFNumberGetValue(number, kCFNumberDoubleType, &value)) {
							MSLog(MSLogLevelError, "Cylo:Error: Unable to Read CoreFoundationVersion[0]: %@", number);
							goto release;
						}

						if (value > kCFCoreFoundationVersionNumber)
							goto release;

						if (count != 1) {
							number = reinterpret_cast<CFNumberRef>(CFArrayGetValueAtIndex(version, 1));

							if (CFGetTypeID(number) != CFNumberGetTypeID() || !CFNumberGetValue(number, kCFNumberDoubleType, &value)) {
								MSLog(MSLogLevelError, "Cylo:Error: Unable to Read CoreFoundationVersion[1]: %@", number);
								goto release;
							}

							if (value <= kCFCoreFoundationVersionNumber)
								goto release;
						}
					}

					load = true;
				}

				bool any;
				if (CFStringRef mode = reinterpret_cast<CFStringRef>(CFDictionaryGetValue(filter, CFSTR("Mode"))))
					any = CFEqual(mode, CFSTR("Any"));
				else
					any = false;

				if (any)
					load = false;

				if (CFArrayRef executables = reinterpret_cast<CFArrayRef>(CFDictionaryGetValue(filter, CFSTR("Executables")))) {
					if (!any)
						load = false;

					CFStringRef name(CFStringCreateWithCStringNoCopy(kCFAllocatorDefault, slash, kCFStringEncodingUTF8, kCFAllocatorNull));

					for (CFIndex i(0), count(CFArrayGetCount(executables)); i != count; ++i) {
						CFStringRef executable(reinterpret_cast<CFStringRef>(CFArrayGetValueAtIndex(executables, i)));
						if (CFEqual(executable, name)) {
							if (ForSaurik)
								MSLog(MSLogLevelNotice, "Cylo:Notice: Found: %@", name);
							load = true;
							break;
						}
					}

					CFRelease(name);

					if (!any && !load)
						goto release;
				}

				if (CFArrayRef bundles = reinterpret_cast<CFArrayRef>(CFDictionaryGetValue(filter, CFSTR("Bundles")))) {
					if (!any)
						load = false;

					for (CFIndex i(0), count(CFArrayGetCount(bundles)); i != count; ++i) {
						CFStringRef bundle(reinterpret_cast<CFStringRef>(CFArrayGetValueAtIndex(bundles, i)));
						if (CFBundleGetBundleWithIdentifier(bundle) != NULL) {
							if (ForSaurik)
								MSLog(MSLogLevelNotice, "Cylo:Notice: Found: %@", bundle);
							load = true;
							break;
						}
					}

					if (!any && !load)
						goto release;
				}

				if (CFArrayRef classes = reinterpret_cast<CFArrayRef>(CFDictionaryGetValue(filter, CFSTR("Classes")))) {
					if (!any)
						load = false;

					if (NSClassFromString != NULL)
						for (CFIndex i(0), count(CFArrayGetCount(classes)); i != count; ++i) {
							CFStringRef _class(reinterpret_cast<CFStringRef>(CFArrayGetValueAtIndex(classes, i)));
							if (NSClassFromString(_class) != NULL) {
								if (ForSaurik)
									MSLog(MSLogLevelNotice, "Cylo:Notice: Found: %@", _class);
								load = true;
								break;
							}
						}

					if (!any && !load)
						goto release;
				}
			}

		  release:
			CFRelease(meta);
		}

		if (!load)
			continue;

		CFURLGetFileSystemRepresentation(script, TRUE, reinterpret_cast<UInt8 *>(path), sizeof(path));
		MSLog(MSLogLevelNotice, "Cylo:Notice: Loading: %s", path);

		FILE *file = fopen(path, "r");
		if (!file) {
			MSLog(MSLogLevelError, "Cylo:Error: Unable to open file with error %d", errno);
			continue;
		}

		if (fseek(file, 0, SEEK_END) != 0) {
			MSLog(MSLogLevelError, "Cylo:Error: Unable to seek to end with error %d", errno);
			fclose(file);
			continue;
		}

		long int fileLength = ftell(file);
		if (fileLength == -1) {
			MSLog(MSLogLevelError, "Cylo:Error: Unable to read file length with error %d", errno);
			fclose(file);
			continue;
		}

		if (fseek(file, 0, SEEK_SET) != 0) {
			MSLog(MSLogLevelError, "Cylo:Error: Unable to seek back to start with error %d", errno);
			fclose(file);
			continue;
		}

		char *buffer = (char *)malloc(fileLength + 1);
		if (!buffer) {
			MSLog(MSLogLevelError, "Cylo:Error: Unable to malloc!");
			fclose(file);
			continue;
		}

		if (fread(buffer, fileLength, 1, file) != 1) {
			MSLog(MSLogLevelError, "Cylo:Error: Unable to read file %d", errno);
			fclose(file);
			free(buffer);
			continue;
		}

		fclose(file);

		JSGlobalContextRef context = JSGlobalContextCreate(NULL);
		CydgetSetupContext(context);

		JSStringRef expression;
		if (path[length - 2] == 'c') {
			CFDataRef data = CFDataCreateWithBytesNoCopy(kCFAllocatorDefault, (const UInt8 *)buffer, fileLength, kCFAllocatorNull);
			CFStringRef string = CFStringCreateFromExternalRepresentation(kCFAllocatorDefault, data, kCFStringEncodingUTF8);
			CFRelease(data);
			if (!string) {
				free(buffer);
				MSLog(MSLogLevelError, "Cylo:Error: Invalid encoding!");
				continue;
			}
			size_t expressionLength = CFStringGetLength(string);
			UniChar *expressionBuffer = (UniChar *)malloc(expressionLength * sizeof(UniChar));
			if (!expressionBuffer) {
				free(buffer);
				MSLog(MSLogLevelError, "Cylo:Error: Unable to malloc!");
				continue;
			}
			CFStringGetCharacters(string, CFRangeMake(0, expressionLength), expressionBuffer);
			CFRelease(string);
			apr_pool_t *pool = NULL;
			apr_pool_create(&pool, NULL);
			const uint16_t *parsedExpressionBuffer = (const uint16_t *)expressionBuffer;
			CydgetPoolParse(pool, &parsedExpressionBuffer, &expressionLength);
			expression = JSStringCreateWithCharacters(parsedExpressionBuffer, expressionLength);
			free((void *)expressionBuffer);
			apr_pool_destroy(pool);
		} else {
			buffer[fileLength] = '\0';
			expression = JSStringCreateWithUTF8CString(buffer);
		}

		JSValueRef exception = NULL;
		JSEvaluateScript(context, expression, NULL, NULL, 0, &exception);
		JSStringRelease(expression);

		if (exception) {
			JSStringRef exceptionString = JSValueToStringCopy(context, exception, NULL);
			CFStringRef exceptionCFString = JSStringCopyCFString(kCFAllocatorDefault, exceptionString);
			JSStringRelease(exceptionString);
			MSLog(MSLogLevelError, "Cylo:Error: Script Error: %@", exceptionCFString);
			CFRelease(exceptionCFString);
		}

		free(buffer);

	}
}
