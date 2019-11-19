//-------------------------------------------------------------------------------------------------------
// Copyright (C) Microsoft Corporation. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------

#include <regex>
#include <vector>

#include <known_folders.h>
#include <objbase.h>
#include <psf_framework.h>
#include <utilities.h>

#include "FunctionImplementations.h"
#include "PathRedirection.h"
#include <TraceLoggingProvider.h>
#include "Telemetry.h"
#include "RemovePII.h"

TRACELOGGING_DECLARE_PROVIDER(g_Log_ETW_ComponentProvider);
TRACELOGGING_DEFINE_PROVIDER(
    g_Log_ETW_ComponentProvider,
    "Microsoft.Windows.PSFRuntime",
    (0xf7f4e8c4, 0x9981, 0x5221, 0xe6, 0xfb, 0xff, 0x9d, 0xd1, 0xcd, 0xa4, 0xe1),
    TraceLoggingOptionMicrosoftTelemetry());

using namespace std::literals;

std::filesystem::path g_packageRootPath;
std::filesystem::path g_packageVfsRootPath;
std::filesystem::path g_redirectRootPath;
std::filesystem::path g_writablePackageRootPath;
std::filesystem::path g_finalPackageRootPath;

struct vfs_folder_mapping
{
    std::filesystem::path path;
    std::filesystem::path package_vfs_relative_path; // E.g. "Windows"
};
std::vector<vfs_folder_mapping> g_vfsFolderMappings;

void InitializePaths()
{
    // For path comparison's sake - and the fact that std::filesystem::path doesn't handle (root-)local device paths all
    // that well - ensure that these paths are drive-absolute
    auto packageRootPath = std::wstring(::PSFQueryPackageRootPath());
    auto pathType = psf::path_type(packageRootPath.c_str());
    if (pathType == psf::dos_path_type::root_local_device || (pathType == psf::dos_path_type::local_device))
    {
        packageRootPath += 4;
    }
    assert(psf::path_type(packageRootPath.c_str()) == psf::dos_path_type::drive_absolute);
    transform(packageRootPath.begin(), packageRootPath.end(), packageRootPath.begin(), towlower);
    g_packageRootPath = psf::remove_trailing_path_separators(packageRootPath);

    g_packageVfsRootPath = g_packageRootPath / L"VFS";

	auto finalPackageRootPath = std::wstring(::PSFQueryFinalPackageRootPath());
	g_finalPackageRootPath = psf::remove_trailing_path_separators(finalPackageRootPath);
    
    // Ensure that the redirected root path exists
    g_redirectRootPath = psf::known_folder(FOLDERID_LocalAppData) / std::filesystem::path(L"Packages") / psf::current_package_family_name() / LR"(LocalCache\Local\VFS)";
    std::filesystem::create_directories(g_redirectRootPath);

    g_writablePackageRootPath = psf::known_folder(FOLDERID_LocalAppData) /std::filesystem::path(L"Packages") / psf::current_package_family_name() / LR"(LocalCache\Local\Microsoft\WritablePackageRoot)";
    std::filesystem::create_directories(g_writablePackageRootPath);

    // Folder IDs and their desktop bridge packaged VFS location equivalents. Taken from:
    // https://docs.microsoft.com/en-us/windows/uwp/porting/desktop-to-uwp-behind-the-scenes
    //      System Location                 Redirected Location (Under [PackageRoot]\VFS)   Valid on architectures
    //      FOLDERID_SystemX86              SystemX86                                       x86, amd64
    //      FOLDERID_System                 SystemX64                                       amd64
    //      FOLDERID_ProgramFilesX86        ProgramFilesX86                                 x86, amd6
    //      FOLDERID_ProgramFilesX64        ProgramFilesX64                                 amd64
    //      FOLDERID_ProgramFilesCommonX86  ProgramFilesCommonX86                           x86, amd64
    //      FOLDERID_ProgramFilesCommonX64  ProgramFilesCommonX64                           amd64
    //      FOLDERID_Windows                Windows                                         x86, amd64
    //      FOLDERID_ProgramData            Common AppData                                  x86, amd64
    //      FOLDERID_System\catroot         AppVSystem32Catroot                             x86, amd64
    //      FOLDERID_System\catroot2        AppVSystem32Catroot2                            x86, amd64
    //      FOLDERID_System\drivers\etc     AppVSystem32DriversEtc                          x86, amd64
    //      FOLDERID_System\driverstore     AppVSystem32Driverstore                         x86, amd64
    //      FOLDERID_System\logfiles        AppVSystem32Logfiles                            x86, amd64
    //      FOLDERID_System\spool           AppVSystem32Spool                               x86, amd64
    g_vfsFolderMappings.push_back(vfs_folder_mapping{ psf::known_folder(FOLDERID_SystemX86),                   LR"(SystemX86)"sv });
    g_vfsFolderMappings.push_back(vfs_folder_mapping{ psf::known_folder(FOLDERID_ProgramFilesX86),             LR"(ProgramFilesX86)"sv });
    g_vfsFolderMappings.push_back(vfs_folder_mapping{ psf::known_folder(FOLDERID_ProgramFilesCommonX86),       LR"(ProgramFilesCommonX86)"sv });
#if !_M_IX86
    // FUTURE: We may want to consider the possibility of a 32-bit application trying to reference "%windir%\sysnative\"
    //         in which case we'll have to get smarter about how we resolve paths
    g_vfsFolderMappings.push_back(vfs_folder_mapping{ psf::known_folder(FOLDERID_System), LR"(SystemX64)"sv });
    // FOLDERID_ProgramFilesX64* not supported for 32-bit applications
    // FUTURE: We may want to consider the possibility of a 32-bit process trying to access this path anyway. E.g. a
    //         32-bit child process of a 64-bit process that set the current directory
    g_vfsFolderMappings.push_back(vfs_folder_mapping{ psf::known_folder(FOLDERID_ProgramFilesX64), LR"(ProgramFilesX64)"sv });
    g_vfsFolderMappings.push_back(vfs_folder_mapping{ psf::known_folder(FOLDERID_ProgramFilesCommonX64), LR"(ProgramFilesCommonX64)"sv });
#endif
    g_vfsFolderMappings.push_back(vfs_folder_mapping{ psf::known_folder(FOLDERID_Windows),                      LR"(Windows)"sv });
    g_vfsFolderMappings.push_back(vfs_folder_mapping{ psf::known_folder(FOLDERID_ProgramData),                  LR"(Common AppData)"sv });
    g_vfsFolderMappings.push_back(vfs_folder_mapping{ psf::known_folder(FOLDERID_System),                       LR"(System)"sv });
    g_vfsFolderMappings.push_back(vfs_folder_mapping{ psf::known_folder(FOLDERID_System) / LR"(catroot)"sv,     LR"(AppVSystem32Catroot)"sv });
    g_vfsFolderMappings.push_back(vfs_folder_mapping{ psf::known_folder(FOLDERID_System) / LR"(catroot2)"sv,    LR"(AppVSystem32Catroot2)"sv });
    g_vfsFolderMappings.push_back(vfs_folder_mapping{ psf::known_folder(FOLDERID_System) / LR"(drivers\etc)"sv, LR"(AppVSystem32DriversEtc)"sv });
    g_vfsFolderMappings.push_back(vfs_folder_mapping{ psf::known_folder(FOLDERID_System) / LR"(driverstore)"sv, LR"(AppVSystem32Driverstore)"sv });
    g_vfsFolderMappings.push_back(vfs_folder_mapping{ psf::known_folder(FOLDERID_System) / LR"(logfiles)"sv,    LR"(AppVSystem32Logfiles)"sv });
    g_vfsFolderMappings.push_back(vfs_folder_mapping{ psf::known_folder(FOLDERID_System) / LR"(spool)"sv,       LR"(AppVSystem32Spool)"sv });
    
    // These are additional folders that may appear in MSIX packages and need help
    g_vfsFolderMappings.push_back(vfs_folder_mapping{ psf::known_folder(FOLDERID_LocalAppData),                 LR"(Local AppData)"sv });
    g_vfsFolderMappings.push_back(vfs_folder_mapping{ psf::known_folder(FOLDERID_RoamingAppData),               LR"(AppData)"sv });

    //These are additional folders seen from App-V packages converted into MSIX (still looking for an official App-V list)
    g_vfsFolderMappings.push_back(vfs_folder_mapping{ psf::known_folder(FOLDERID_Fonts),                        LR"(Fonts)"sv });
    g_vfsFolderMappings.push_back(vfs_folder_mapping{ psf::known_folder(FOLDERID_PublicDesktop),                LR"(Common Desktop)"sv });
    g_vfsFolderMappings.push_back(vfs_folder_mapping{ psf::known_folder(FOLDERID_CommonPrograms),               LR"(Common Programs)"sv });
    g_vfsFolderMappings.push_back(vfs_folder_mapping{ psf::known_folder(FOLDERID_LocalAppDataLow),              LR"(LOCALAPPDATALOW)"sv });
}

std::filesystem::path path_from_known_folder_string(std::wstring_view str)
{
    KNOWNFOLDERID id;

    if (str == L"SystemX86"sv)
    {
        id = FOLDERID_SystemX86;
    }
    else if (str == L"System"sv)
    {
        id = FOLDERID_System;
    }
    else if (str == L"ProgramFilesX86"sv)
    {
        id = FOLDERID_ProgramFilesX86;
    }
    else if (str == L"ProgramFilesCommonX86"sv)
    {
        id = FOLDERID_ProgramFilesCommonX86;
    }
#if _M_IX86
    else if ((str == L"ProgramFilesX64"sv) || (str == L"ProgramFilesCommonX64"sv))
    {
        return {};
    }
#else
    else if (str == L"ProgramFilesX64"sv)
    {
        id = FOLDERID_ProgramFilesX64;
    }
    else if (str == L"ProgramFilesCommonX64"sv)
    {
        id = FOLDERID_ProgramFilesCommonX64;
    }
#endif
    else if (str == L"Windows"sv)
    {
        id = FOLDERID_Windows;
    }
    else if (str == L"ProgramData"sv)
    {
        id = FOLDERID_ProgramData;
    }
    else if (str == L"LocalAppData"sv)
    {
        id = FOLDERID_LocalAppData;
    }
    else if (str == L"RoamingAppData"sv)
    {
        id = FOLDERID_RoamingAppData;
    }
    else if ((str.length() >= 38) && (str[0] == '{'))
    {
        if (FAILED(::IIDFromString(str.data(), &id)))
        {
            return {};
        }
    }
    else
    {
        // Unknown
        return {};
    }

    return psf::known_folder(id);
}

struct path_redirection_spec
{
    std::filesystem::path base_path;
    std::wregex pattern;
    std::filesystem::path redirect_targetbase;
    bool isExclusion;
    bool isReadOnly;
};

std::vector<path_redirection_spec> g_redirectionSpecs;




void Log(const char* fmt, ...)
{

    va_list args;
    va_start(args, fmt);
    std::string str;
    str.resize(256);
    std::size_t count = std::vsnprintf(str.data(), str.size() + 1, fmt, args);
    assert(count >= 0);
    va_end(args);

    if (count > str.size())
    {
        str.resize(count);

        va_list args2;
        va_start(args2, fmt);
        count = std::vsnprintf(str.data(), str.size() + 1, fmt, args2);
        assert(count >= 0);
        va_end(args2);
    }

    str.resize(count);
#if _DEBUG
    ::OutputDebugStringA(str.c_str());
#endif
}
void Log(const wchar_t* fmt, ...)  
{
    try
    {
        va_list args;
        va_start(args, fmt);

        std::wstring wstr;
        wstr.resize(256);
        std::size_t count = std::vswprintf(wstr.data(), wstr.size() + 1, fmt, args);
        va_end(args);
        assert(count >= 0);

        if (count > wstr.size())
        {
            wstr.resize(count);
            va_list args2;
            va_start(args2, fmt);
            count = std::vswprintf(wstr.data(), wstr.size() + 1, fmt, args2);
            va_end(args2);
            assert(count >= 0);
        }
        wstr.resize(count);
#if _DEBUG
        ::OutputDebugStringW(wstr.c_str());
		::OutputDebugStringW(L"\n");
#endif
    }
    catch (...)
    {
        ::OutputDebugStringA("Exception in wide Log()");
    }
}

template <typename CharT>
bool IsUnderUserAppDataLocalImpl(_In_ const CharT* fileName)
{
    return path_relative_to(fileName, psf::known_folder(FOLDERID_LocalAppData));
}
bool IsUnderUserAppDataLocal(_In_ const wchar_t* fileName)
{
    return IsUnderUserAppDataLocalImpl(fileName);
}
bool IsUnderUserAppDataLocal(_In_ const char* fileName)
{
    return IsUnderUserAppDataLocalImpl(fileName);
}

template <typename CharT>
bool IsUnderUserAppDataRoamingImpl(_In_ const CharT* fileName)
{
    return path_relative_to(fileName, psf::known_folder(FOLDERID_RoamingAppData));
}
bool IsUnderUserAppDataRoaming(_In_ const wchar_t* fileName)
{
    return IsUnderUserAppDataRoamingImpl(fileName);
}
bool IsUnderUserAppDataRoaming(_In_ const char* fileName)
{
    return IsUnderUserAppDataRoamingImpl(fileName);
}

template <typename CharT>
std::filesystem::path GetPackageVFSPathImpl(const CharT* fileName)
{
    if (IsUnderUserAppDataLocal(fileName))
    {
        auto lad = psf::known_folder(FOLDERID_LocalAppData);
        std::filesystem::path foo = fileName; 
        auto testLad = g_packageVfsRootPath / L"Local AppData" / foo.wstring().substr(wcslen(lad.c_str())+1).c_str();
        return testLad;
    }
    else if (IsUnderUserAppDataRoaming(fileName))
    {
        auto rad = psf::known_folder(FOLDERID_RoamingAppData);
        std::filesystem::path foo = fileName;
        auto testRad = g_packageVfsRootPath / L"AppData" / foo.wstring().substr(wcslen(rad.c_str())+1).c_str();
        return testRad;
    }
    return L"";
}
std::filesystem::path GetPackageVFSPath(const wchar_t* fileName)
{
    return GetPackageVFSPathImpl(fileName);
}
std::filesystem::path GetPackageVFSPath(const char* fileName)
{
    return GetPackageVFSPathImpl(fileName);
}

void InitializeConfiguration()
{
    TraceLoggingRegister(g_Log_ETW_ComponentProvider);
    std::wstringstream traceDataStream;

    if (auto rootConfig = ::PSFQueryCurrentDllConfig())
    {
        auto& rootObject = rootConfig->as_object();
        traceDataStream << " config:\n";
        if (auto pathsValue = rootObject.try_get("redirectedPaths"))
        {
            traceDataStream << " redirectedPaths:\n";
            auto& redirectedPathsObject = pathsValue->as_object();
            auto initializeRedirection = [&traceDataStream](const std::filesystem::path & basePath, const psf::json_array & specs, bool traceOnly = false)
            {
                for (auto& spec : specs)
                {
                    auto& specObject = spec.as_object();
                    auto path = psf::remove_trailing_path_separators(basePath / specObject.get("base").as_string().wstring());
                    std::filesystem::path redirectTargetBaseValue = g_writablePackageRootPath;
                    if (auto redirectTargetBase = specObject.try_get("redirectTargetBase"))
                    {
                        redirectTargetBaseValue = specObject.get("redirectTargetBase").as_string().wstring();	
                    }
                    bool IsExclusionValue = false;
                    if (auto IsExclusion = specObject.try_get("isExclusion"))
                    {
                        IsExclusionValue = specObject.get("isExclusion").as_boolean().get();
                    }
                    bool IsReadOnlyValue = false;
                    if (auto IsReadOnly = specObject.try_get("isReadOnly"))
                    {
                        IsReadOnlyValue = specObject.get("isReadOnly").as_boolean().get();
                    }
                  
                    traceDataStream << " base:" << RemovePIIfromFilePath(specObject.get("base").as_string().wide()) << " ;";
                    traceDataStream << " patterns:";
                    for (auto& pattern : specObject.get("patterns").as_array())
                    {
                        auto patternString = pattern.as_string().wstring();
                        traceDataStream << pattern.as_string().wide() << " ;";                      
                        if (!traceOnly)
                        {
                          g_redirectionSpecs.emplace_back();
                          g_redirectionSpecs.back().base_path = path;
                          g_redirectionSpecs.back().pattern.assign(patternString.data(), patternString.length());
                          g_redirectionSpecs.back().redirect_targetbase = redirectTargetBaseValue;
                          g_redirectionSpecs.back().isExclusion = IsExclusionValue;
                          g_redirectionSpecs.back().isReadOnly = IsReadOnlyValue;
                        }
                    }
                    Log(L"\t\tFRF RULE: Path=%ls retarget=%ls", path.c_str(), redirectTargetBaseValue.c_str());
                }
            };

            if (auto packageRelativeValue = redirectedPathsObject.try_get("packageRelative"))
            {
                traceDataStream << " packageRelative:\n";
                initializeRedirection(g_packageRootPath, packageRelativeValue->as_array());
            }

            if (auto packageDriveRelativeValue = redirectedPathsObject.try_get("packageDriveRelative"))
            {
                traceDataStream << " packageDriveRelative:\n";
                initializeRedirection(g_packageRootPath.root_name(), packageDriveRelativeValue->as_array());
            }

            if (auto knownFoldersValue = redirectedPathsObject.try_get("knownFolders"))
            {
                traceDataStream << " knownFolders:\n";
                for (auto& knownFolderValue : knownFoldersValue->as_array())
                {
                    auto& knownFolderObject = knownFolderValue.as_object();
                    auto path = path_from_known_folder_string(knownFolderObject.get("id").as_string().wstring());
                    traceDataStream << " id:" << knownFolderObject.get("id").as_string().wide() << " ;";

                    traceDataStream << " relativePaths:\n";
                    initializeRedirection(path, knownFolderObject.get("relativePaths").as_array(), path.empty());
                }
            }
        }

        TraceLoggingWrite(
            g_Log_ETW_ComponentProvider,
            "FileRedirectionFixupConfigdata",
            TraceLoggingWideString(traceDataStream.str().c_str(), "FileRedirectionFixupConfig"),
            TraceLoggingBoolean(TRUE, "UTCReplace_AppSessionGuid"),
            TelemetryPrivacyDataTag(PDT_ProductAndServiceUsage),
            TraceLoggingKeyword(MICROSOFT_KEYWORD_CRITICAL_DATA));
    }

    TraceLoggingUnregister(g_Log_ETW_ComponentProvider);
}

template <typename CharT>
bool path_relative_toImpl(const CharT* path, const std::filesystem::path& basePath)
{
    return std::equal(basePath.native().begin(), basePath.native().end(), path, psf::path_compare{});
}
bool path_relative_to(const wchar_t* path, const std::filesystem::path& basePath)
{
    return path_relative_toImpl(path, basePath);
}
bool path_relative_to(const char* path, const std::filesystem::path& basePath)
{
    return path_relative_toImpl(path, basePath);
}

bool IsColonColonGuid(const char *path)
{
    if (strlen(path) > 39)
    {
        if (path[0] == ':' &&
            path[1] == ':' &&
            path[2] == '{')
        {
            return true;
        }
    }
    return false;
}
bool IsColonColonGuid(const wchar_t *path)
{
    if (wcslen(path) > 39)
    {
        if (path[0] == L':' &&
            path[1] == L':' &&
            path[2] == L'{'   )
        {
            return true;
        }
    }
    return false;
}

bool IsBlobColon(const std::string path)
{
    size_t found = path.find("blob:", 0);
    if (found == 0)
        return true;
    found = path.find("BLOB:", 0);
    if (found == 0)
        return true;
    return false;
}
bool IsBlobColon(const std::wstring path)
{
    size_t found = path.find(L"blob:", 0);
    if (found == 0)
        return true;
    found = path.find(L"BLOB:", 0);
    if (found == 0)
        return true;
    return false;
}

std::string UrlDecode(std::string str) 
{
    std::string ret;
    char ch;
    size_t i,  len = str.length();
    unsigned int ii;

    for (i = 0; i < len; i++) 
    {
        if (str[i] != '%') 
        {
            ret += str[i];
        }
        else 
        {
            sscanf_s(str.substr(i + 1, 2).c_str(), "%x", &ii);
            ch = static_cast<char>(ii);
            ret += ch;
            i = i + 2;
        }
    }
    return ret;
}

std::wstring UrlDecode(std::wstring str) 
{
    std::wstring ret;
    char ch;
    size_t i,  len = str.length();
    unsigned int ii;

    for (i = 0; i < len; i++) 
    {
        if (str[i] != L'%') 
        {
            ret += str[i];
        }
        else 
        {
            swscanf_s(str.substr(i + 1, 2).c_str(), L"%x", &ii);
            ch = static_cast<char>(ii);
            ret += ch;
            i = i + 2;
        }
    }
    return ret;
}

std::string StripFileColonSlash(std::string old_string)
{
    size_t found = old_string.find("file:\\",0);
    if (found != 0)
    {
        found = old_string.find("file:/", 0);
    }
    if (found != 0)
    {
        found = old_string.find("FILE:\\", 0);
    }
    if (found != 0)
    {
        found = old_string.find("FILE:/", 0);
    }
    if (found == 0)
    {
        return old_string.substr(6);
    }
    return old_string;
}
std::wstring StripFileColonSlash(std::wstring old_string)
{
    size_t found = old_string.find(L"file:\\", 0);
    if (found != 0)
    {
        found = old_string.find(L"file:/", 0);
    }
    if (found != 0)
    {
        found = old_string.find(L"FILE:\\", 0);
    }
    if (found != 0)
    {
        found = old_string.find(L"FILE:/", 0);
    }
    if (found == 0)
    {
        return old_string.substr(6);
    }
    return old_string;
}

template <typename CharT>
normalized_path NormalizePathImpl(const CharT* path)
{
    normalized_path result;

    auto pathType = psf::path_type(path);
    if (pathType == psf::dos_path_type::root_local_device)
    {
        // Root-local device paths are a direct escape into the object manager, so don't normalize them
        result.full_path = widen(path);
    }
    else if (pathType != psf::dos_path_type::unknown)
    {
        result.full_path = widen(psf::full_path(path));
        pathType = psf::path_type(result.full_path.c_str());
    }
    else // unknown
    {
        return result;
    }

    if (pathType == psf::dos_path_type::drive_absolute)
    {
        result.drive_absolute_path = result.full_path.data();
    }
    else if ((pathType == psf::dos_path_type::local_device) || (pathType == psf::dos_path_type::root_local_device))
    {
        auto candidatePath = result.full_path.data() + 4;
        if (psf::path_type(candidatePath) == psf::dos_path_type::drive_absolute)
        {
            result.drive_absolute_path = candidatePath;
        }
    }
    else if (pathType == psf::dos_path_type::unc_absolute)
    {
        // We assume that UNC paths will never reference a path that we need to redirect. Note that this isn't perfect.
        // E.g. "\\localhost\C$\foo\bar.txt" is the same path as "C:\foo\bar.txt"; we shall defer solving this problem
        return result;
    }
    else
    {
        // GetFullPathName did something odd...
        Log(L"\t\tFRF Error: Path=%ls unknown", path);
        assert(false);
        return {};
    }

    return result;
}

normalized_path NormalizePath(const char* path)
{
    if (path != NULL && path[0] != 0)
    {
        std::string new_string = path;
        if (IsColonColonGuid(path))
        {
            Log(L"Guid: avoidance");
            normalized_path npath;
            npath.full_path = widen(new_string);
            return npath;
        }
        if (IsBlobColon(new_string))  // blog:hexstring has been seen, believed to be associated with writing encrypted data,  Just pass it through as it is not a real file.
        {
            Log(L"Blob: avoidance");
            normalized_path npath;
            npath.full_path = widen(new_string);
            return npath;
        }      
        new_string = UrlDecode(path);       // replaces things like %3a with :
        new_string = StripFileColonSlash(new_string);        // removes file:\\ from start of path if present
        return NormalizePathImpl(new_string.c_str());
    }
    else
    {
        return NormalizePathImpl(".");
    }
}

normalized_path NormalizePath(const wchar_t* path)
{
    if (path != NULL && path[0] != 0)
    {
        std::wstring new_wstring = path;
        if (IsColonColonGuid(path))
        {
            Log(L"Guid: avoidance");
            normalized_path npath;
            npath.full_path = widen(new_wstring);
            return npath;
        }
        if (IsBlobColon(new_wstring))  // blog:hexstring has been seen, believed to be associated with writing encrypted data,  Just pass it through as it is not a real file.
        {
            Log(L"Blob: avoidance");
            normalized_path npath;
            npath.full_path = widen(new_wstring);
            return npath;
        }        
        new_wstring = UrlDecode(path);                      // replaces things like %3a with :
        new_wstring = StripFileColonSlash(new_wstring);     // removes file:\\ from start of path if present
        return NormalizePathImpl(new_wstring.c_str());
    }
    else
    {
        return NormalizePathImpl(L".");
    }
}



// If the input path is relative to the VFS folder under the package path (e.g. "${PackageRoot}\VFS\SystemX64\foo.txt"),
// then modifies that path to its virtualized equivalent (e.g. "C:\Windows\System32\foo.txt")
normalized_path DeVirtualizePath(normalized_path path)
{
    if (path.drive_absolute_path && path_relative_to(path.drive_absolute_path, g_packageVfsRootPath))
    {
        auto packageRelativePath = path.drive_absolute_path + g_packageVfsRootPath.native().length();
        if (psf::is_path_separator(packageRelativePath[0]))
        {
            ++packageRelativePath;
            for (auto& mapping : g_vfsFolderMappings)
            {
                if (path_relative_to(packageRelativePath, mapping.package_vfs_relative_path))
                {
                    auto vfsRelativePath = packageRelativePath + mapping.package_vfs_relative_path.native().length();
                    if (psf::is_path_separator(vfsRelativePath[0]))
                    {
                        ++vfsRelativePath;
                    }
                    else if (vfsRelativePath[0])
                    {
                        // E.g. AppVSystem32Catroot2, but matched with AppVSystem32Catroot. This is not the match we are
                        // looking for
                        continue;
                    }

                    // NOTE: We should have already validated that mapping.path is drive-absolute
                    path.full_path = (mapping.path / vfsRelativePath).native();
                    path.drive_absolute_path = path.full_path.data();
                    break;
                }
            }
        }
        // Otherwise a directory/file named something like "VFSx" for some non-path separator/null terminator 'x'
    }

    return path;
}

// If the input path is a physical path outside of the package (e.g. "C:\Windows\System32\foo.txt"),
// this returns what the package VFS equivalent would be (e.g "C:\Program Files\WindowsApps\Packagename\VFS\SystemX64\foo.txt");
// NOTE: Does not check if package has this virtualized path.
normalized_path VirtualizePath(normalized_path path)
{
    //Log(L"\t\tVirtualizePath: Input drive_absolute_path %ls", path.drive_absolute_path);

    if (path.drive_absolute_path && path_relative_to(path.drive_absolute_path, g_packageRootPath))
    {
        Log(L"\t\tVirtualizePath: output same as input, is in package");
        return path;
    }
    
    for (std::vector<vfs_folder_mapping>::reverse_iterator iter = g_vfsFolderMappings.rbegin();   iter != g_vfsFolderMappings.rend(); ++iter)
    {
        auto& mapping = *iter;
        if (path_relative_to(path.full_path.c_str(), mapping.path))
        {
            Log(L"\t\t\t mapping entry match on path %ls", mapping.path.wstring().c_str());
            Log(L"\t\t\t package_vfs_relative_path %ls", mapping.package_vfs_relative_path.native().c_str());
            Log(L"\t\t\t rel length =%d, %d", mapping.path.native().length(), mapping.package_vfs_relative_path.native().length());
            auto vfsRelativePath = path.full_path.c_str() + mapping.path.native().length();
            if (psf::is_path_separator(vfsRelativePath[0]))
            {
                ++vfsRelativePath;
            }
            Log(L"\t\t\t vfsRelativePath %ls", vfsRelativePath);
            path.full_path = (g_packageVfsRootPath / mapping.package_vfs_relative_path / vfsRelativePath).native();
            path.drive_absolute_path = path.full_path.data();
            return path;
        }
    }
    Log(L"\t\tVirtualizePath: output same as input, no match.");
    return path;
}

std::wstring GenerateRedirectedPath(std::wstring_view relativePath, bool ensureDirectoryStructure, std::wstring result)
{
    if (ensureDirectoryStructure)
    {
        for (std::size_t pos = 0; pos < relativePath.length(); )
        {
            Log(L"\t\tCreate dir: %ls", result.c_str());
            [[maybe_unused]] auto dirResult = impl::CreateDirectory(result.c_str(), nullptr);
#if _DEBUG
            auto err = ::GetLastError();
            assert(dirResult || (err == ERROR_ALREADY_EXISTS));
#endif
            auto nextPos = relativePath.find_first_of(LR"(\/)", pos + 1);
            if (nextPos == relativePath.length())
            {
                // Ignore trailing path separators. E.g. if the call is to CreateDirectory, we don't want it to "fail"
                // with an "already exists" error
                nextPos = std::wstring_view::npos;
            }

            result += relativePath.substr(pos, nextPos - pos);
            pos = nextPos;
        }
    }
    else
    {
        result += relativePath;
    }

    return result;
}
/// <summary>
/// Figures out the absolute path to redirect to.
/// </summary>
/// <param name="deVirtualizedPath">The original path from the app</param>
/// <param name="ensureDirectoryStructure">If true, the deVirtualizedPath will be appended to the allowed write location</param>
/// <returns>The new absolute path.</returns>
std::wstring RedirectedPath(const normalized_path& deVirtualizedPath, bool ensureDirectoryStructure, std::filesystem::path destinationTargetBase)
{
    std::wstring result;
    std::wstring basePath;
    std::wstring relativePath;

    bool shouldredirectToPackageRoot = false;
    auto deVirtualizedFullPath = deVirtualizedPath.full_path;

    ///if (_wcsicmp(destinationTargetBase.c_str(), g_redirectRootPath.c_str()) == 0)
    if (_wcsicmp(destinationTargetBase.c_str(), g_writablePackageRootPath.c_str()) == 0)
    {
        // PSF defaulted destination target.
        basePath = LR"(\\?\)" + g_writablePackageRootPath.native();
    }
    else
    {
        std::filesystem::path destNoTrailer = psf::remove_trailing_path_separators(destinationTargetBase);
        basePath = LR"(\\?\)" + destNoTrailer.wstring();
    }

    //Lowercase the full path because .find is case-sensitive.
    transform(deVirtualizedFullPath.begin(), deVirtualizedFullPath.end(), deVirtualizedFullPath.begin(), towlower);
 
    if (deVirtualizedFullPath.find(g_packageRootPath) != std::wstring::npos)
    {
        Log(L"case: target in package.");
        Log(L"      destinationTargetBase:     %ls", destinationTargetBase.c_str());
        Log(L"      g_writablePackageRootPath: %ls", g_writablePackageRootPath.c_str());

		size_t lengthPackageRootPath = 0;
		auto pathType = psf::path_type(deVirtualizedFullPath.c_str());

		if (pathType == psf::dos_path_type::drive_absolute)
		{
			lengthPackageRootPath = g_packageRootPath.native().length();
		}
		else
		{
			lengthPackageRootPath = g_finalPackageRootPath.native().length();
		}

        if (_wcsicmp(destinationTargetBase.c_str(), g_writablePackageRootPath.c_str()) == 0)
        {
            Log(L"subcase: redirect to default.");
            // PSF defaulted destination target.
            shouldredirectToPackageRoot = true;
            auto stringToTurnIntoAStringView = deVirtualizedPath.full_path.substr(lengthPackageRootPath);
            relativePath = std::wstring_view(stringToTurnIntoAStringView);
        }
        else
        {
            Log(L"subcase: redirect specified.");
            // PSF configured destination target: probably a home drive.
            relativePath = L"\\PackageCache\\" + psf::current_package_family_name() +  deVirtualizedPath.full_path.substr(lengthPackageRootPath);
        }
    }
    else
    {
        Log(L"case: target not in package.");
        Log(L"      destinationTargetBase: %ls", destinationTargetBase.c_str());
        Log(L"      g_redirectRootPath:    %ls", g_redirectRootPath.c_str());
        // input location was not in package path.
            // TODO: Currently, this code redirects always.  We probably don't want to do that!
            //       Ideally, we should look closer at the request; the text below is an example of what might be needed.
            //       If the user asked for a native path and we aren't VFSing close to that path, and it's just a read, we probably shouldn't redirect.
            //       But let's say it was a write, then probably still don't redirect and let the chips fall where they may.
            //       But if we have a VFS folder in the package (such as VFS\AppDataCommon\Vendor) with files and the app tries to add a new file using native pathing, then we probably want to redirect.
            //       There are probably more situations to consider.
            // To avoid redirecting everything with the current implementation, the configuration spec should be as specific as possible so that we never get here.
        if (_wcsicmp(destinationTargetBase.c_str(), g_redirectRootPath.c_str()) == 0)
        {
            Log(L"subcase: redirect to default.");
            // PSF defaulted destination target.
            relativePath = L"\\";
        }
        else
        {
            Log(L"subcase: redirect specified.");
            // PSF  configured destination target: probably a home drive.
            relativePath = L"\\PackageCache\\" + psf::current_package_family_name() + + L"\\VFS\\PackageDrive";
        }

        // NTFS doesn't allow colons in filenames, so simplest thing is to just substitute something in; use a dollar sign
        // similar to what's done for UNC paths
        assert(psf::path_type(deVirtualizedPath.drive_absolute_path) == psf::dos_path_type::drive_absolute);
        relativePath.push_back(L'\\');
        relativePath.push_back(deVirtualizedPath.drive_absolute_path[0]);
        relativePath.push_back('$');
        auto remainingLength = wcslen(deVirtualizedPath.drive_absolute_path);
        remainingLength -= 2;

        relativePath.append(deVirtualizedPath.drive_absolute_path + 2, remainingLength); 
    }

    ////Log(L"\tFRF devirt.full_path %ls", deVirtualizedPath.full_path.c_str());
    ////Log(L"\tFRF devirt.da_path %ls", deVirtualizedPath.drive_absolute_path);
    Log(L"\tFRF initial basePath=%ls relative=%ls", basePath.c_str(),relativePath.c_str());

    // Create folder structure, if needed
    if (impl::PathExists( (basePath +  relativePath).c_str()))
    {
        result = basePath + relativePath;
        Log(L"\t\tFRF Found that a copy exists in the redirected area so we skip the folder creation.");
    }
    else
    {
        std::wstring_view relativePathview = std::wstring_view(relativePath);

        if (shouldredirectToPackageRoot)
        {
            result = GenerateRedirectedPath(relativePath, ensureDirectoryStructure, basePath);
            Log(L"\t\tFRF shouldredirectToPackageRoot case returns %ls.", result.c_str());
        }
        else
        {
            result = GenerateRedirectedPath(relativePath, ensureDirectoryStructure, basePath);
            Log(L"\t\tFRF not to PackageRoot case returns %ls.", result.c_str());
        }       
       
    }
    return result;
}

std::wstring RedirectedPath(const normalized_path& deVirtualizedPath, bool ensureDirectoryStructure)
{
    // Only until all code paths use the new version of the interface...
    return RedirectedPath(deVirtualizedPath, ensureDirectoryStructure, g_writablePackageRootPath.native());
}

template <typename CharT>
static path_redirect_info ShouldRedirectImpl(const CharT* path, redirect_flags flags)
{
    path_redirect_info result;

    if (!path)
    {
        return result;
    }

    Log(L"\tFRF Should: for %ls", path);
    bool c_presense = flag_set(flags, redirect_flags::check_file_presence);
    bool c_copy = flag_set(flags, redirect_flags::copy_file);
    bool c_ensure = flag_set(flags, redirect_flags::ensure_directory_structure);
    Log(L"\t\tFRF flags  CheckPresense:%d  CopyFile:%d  EnsureDirectory:%d", c_presense, c_copy, c_ensure);

    // normalizedPath represents the requested path, redirected to the external system if relevant, or just as requested if not.
    // vfsPath represents this as a package relative path
    auto normalizedPath = NormalizePath(path);
    std::filesystem::path destinationTargetBase;

    if (!normalizedPath.drive_absolute_path)
    {
        // FUTURE: We could do better about canonicalising paths, but the cost/benefit doesn't make it worth it right now
        return result;
    }

    Log(L"\t\tFRF Normalized=%ls", normalizedPath.drive_absolute_path);
    // To be consistent in where we redirect files, we need to map VFS paths to their non-package-relative equivalent
    normalizedPath = DeVirtualizePath(std::move(normalizedPath));

    Log(L"\t\tFRF DeVirtualized=%ls", normalizedPath.drive_absolute_path);

	// If you change the below logic, or
	// you you change what goes into RedirectedPath
	// you need to mirror all changes in FindFirstFileFixup.cpp
	
	// Basically, what goes into RedirectedPath here also needs to go into 
	// FindFirstFileFixup.cpp
    auto vfspath = NormalizePath(path);
    vfspath = VirtualizePath(std::move(vfspath));
    if (vfspath.drive_absolute_path != NULL)
    {
        Log(L"\t\tFRF Virtualized=%ls", vfspath.drive_absolute_path);
    }

    // Figure out if this is something we need to redirect
    for (auto& redirectSpec : g_redirectionSpecs)
    {
        Log(L"\t\tFRF Check against: base:%ls", redirectSpec.base_path.c_str());
        if (path_relative_to(vfspath.drive_absolute_path, redirectSpec.base_path))
        {
            Log(L"\t\tFRF In ball park of base %ls", redirectSpec.base_path.c_str());
            auto relativePath = vfspath.drive_absolute_path + redirectSpec.base_path.native().length();
            if (psf::is_path_separator(relativePath[0]))
            {
                ++relativePath;
            }
            else if (relativePath[0])
            {
                // Otherwise, just a substring match (e.g. we're trying to match against 'foo' but input was 'foobar')
                continue;
            }
            // Otherwise exact match. Assume an implicit directory separator at the end (e.g. for matches to satisfy the
            // first call to CreateDirectory
            Log(L"\t\t\tFRF relativePath=%ls",relativePath);
            if (std::regex_match(relativePath, redirectSpec.pattern))
            {
                if (redirectSpec.isExclusion)
                {
                    // The impact on isExclusion is that redirection is not needed.
                    result.should_redirect = false;
                    Log(L"\t\tFRF CASE:Exclusion for %ls", path);
                }
                else
                {
                    result.should_redirect = true;
                    result.shouldReadonly = (redirectSpec.isReadOnly == true);			

                    // Check if file exists as VFS path in the package
                    if (impl::PathExists(vfspath.drive_absolute_path))
                    {
                        Log(L"\t\t\tFRF CASE:match, existing in package.");
                        destinationTargetBase = redirectSpec.redirect_targetbase;
                        result.redirect_path = RedirectedPath(vfspath, flag_set(flags, redirect_flags::ensure_directory_structure), destinationTargetBase);
                    }
                    else
                    {
                        Log(L"\t\t\tFRF CASE:match, not existing in package.");   // still might want to redirect anyway
                        destinationTargetBase = redirectSpec.redirect_targetbase;
                        //result.redirect_path = RedirectedPath(normalizedPath, flag_set(flags, redirect_flags::ensure_directory_structure), destinationTargetBase);
                        result.redirect_path = RedirectedPath(vfspath, flag_set(flags, redirect_flags::ensure_directory_structure), destinationTargetBase);
                    }
                    Log(L"\t\tFRF CASE:match to %ls", result.redirect_path.c_str());
                }
                break;
            }
            else
            {
                Log(L"\t\tFRF no match on parse %ls", relativePath);
            }
        }
        else
        {
            Log(L"\t\tFRF Not in ball park of base %ls", redirectSpec.base_path.c_str());
        }
    }

    Log(L"\t\tFRF post check 1");


    if (!result.should_redirect)
    {
        Log(L"\tFRF no redirect rule for %ls", path);
        return result;
    }

    Log(L"\t\tFRF post check 2");

    if (flag_set(flags, redirect_flags::check_file_presence))
    {
        if (!impl::PathExists(result.redirect_path.c_str()) &&
            !impl::PathExists(vfspath.drive_absolute_path) &&
            !impl::PathExists(normalizedPath.drive_absolute_path))
        {
            result.should_redirect = false;
            result.redirect_path.clear();
            Log(L"\tFRF skipped (redirected not present check failed) for %ls", path);
            return result;
        }
    }

    Log(L"\t\tFRF post check 3");

    if (flag_set(flags, redirect_flags::copy_file))
    {
        Log(L"\t\tFRF post check 4");
        [[maybe_unused]] BOOL copyResult = false;
        if (impl::PathExists(result.redirect_path.c_str()))
        {
            Log(L"\t\tFRF Found that a copy exists in the redirected area so we skip the folder creation.");
        }
        else
        {
            std::filesystem::path CopySource = normalizedPath.drive_absolute_path;
            if (impl::PathExists(vfspath.drive_absolute_path))
            {
                CopySource = vfspath.drive_absolute_path;
            }


            auto attr = impl::GetFileAttributes(CopySource.c_str()); //normalizedPath.drive_absolute_path);
            Log(L"\t\tFRF source attributes=0x%x", attr);
            if ((attr & FILE_ATTRIBUTE_DIRECTORY) != FILE_ATTRIBUTE_DIRECTORY)
            {
                copyResult = impl::CopyFileEx(
                    CopySource.c_str(), //normalizedPath.drive_absolute_path,
                    result.redirect_path.c_str(),
                    nullptr,
                    nullptr,
                    nullptr,
                    COPY_FILE_FAIL_IF_EXISTS | COPY_FILE_NO_BUFFERING);
                if (copyResult)
                {
                    Log(L"\t\tFRF CopyFile Success %ls %ls", CopySource.c_str(), result.redirect_path.c_str());
                }
                else
                {
                    Log(L"\t\tFRF CopyFile Fail=0x%x %ls %ls", ::GetLastError(), CopySource.c_str(), result.redirect_path.c_str());
                    auto err = ::GetLastError();
                    switch (err)
                    {
                    case ERROR_FILE_EXISTS:
                        Log(L"\t\tFRF  was ERROR_FILE_EXISTS");
                        break;
                    case ERROR_PATH_NOT_FOUND:
                        Log(L"\t\tFRF  was ERROR_PATH_NOT_FOUND");
                        break;
                    case ERROR_FILE_NOT_FOUND:
                        Log(L"\t\tFRF  was ERROR_FILE_NOT_FOUND");
                        break;
                    case ERROR_ALREADY_EXISTS:
                        Log(L"\t\tFRF  was ERROR_ALREADY_EXISTS");
                        break;
                    default:
                        Log(L"\t\tFRF was 0x%x", err);
                        break;
                    }
                }
            }
            else
            {
                copyResult = impl::CreateDirectoryEx(CopySource.c_str(), result.redirect_path.c_str(), nullptr);
                if (copyResult)
                    Log(L"\t\tFRF CreateDir Success %ls %ls", CopySource.c_str(), result.redirect_path.c_str());
                else
                    Log(L"\t\tFRF CreateDir Fail=0x%x %ls %ls", ::GetLastError(), CopySource.c_str(), result.redirect_path.c_str());
#if _DEBUG
                auto err = ::GetLastError();
                assert(copyResult || (err == ERROR_FILE_EXISTS) || (err == ERROR_PATH_NOT_FOUND) || (err == ERROR_FILE_NOT_FOUND) || (err == ERROR_ALREADY_EXISTS));
#endif
            }
        }
        Log(L"\t\tFRF post check 6");
    }
    
    Log(L"\t\tFRF post check 7");
    //Log(L"\tFRF Redirect from %ls", path);
    Log(L"\tFRF Should: Redirect to %ls", result.redirect_path.c_str());

    return result;
}

path_redirect_info ShouldRedirect(const char* path, redirect_flags flags)
{
    return ShouldRedirectImpl(path, flags);
}

path_redirect_info ShouldRedirect(const wchar_t* path, redirect_flags flags)
{
    return ShouldRedirectImpl(path, flags);
}
