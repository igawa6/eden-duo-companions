// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <map>
#include <mutex>
#include "acnh_tex.h"

namespace sw_ui_images {
// Images live on the module instance; the asset worker can overlap configure/sample.
class Images {
public:
    bool Load(const EdenDsmodHostApi& host, const char* key, void* receiver,
              EdenDsmodImageSink sink, bool& handled);
    void Reset();
private:
    std::mutex mutex_;
    std::map<std::string, acnh::Image> cache_;
};
} // namespace sw_ui_images
