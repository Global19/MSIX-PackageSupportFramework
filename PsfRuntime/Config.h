//-------------------------------------------------------------------------------------------------------
// Copyright (C) Microsoft Corporation. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------
#pragma once

#include <filesystem>
#include <string>

void LoadConfig();

// Globals set by `LoadConfig`, to avoid continuously querying them
const std::wstring& PackageFullName() noexcept;
const std::wstring& ApplicationUserModelId() noexcept;
const std::wstring& ApplicationId() noexcept;
const std::filesystem::path& PackageRootPath() noexcept;
const std::filesystem::path& FinalPackageRootPath() noexcept;
