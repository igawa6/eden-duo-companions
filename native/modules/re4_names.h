// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "re4_assets.h"
#include <array>
namespace re4 {
class RuntimeNames {
public:
    void Load(const EdenDsmodHostApi* host,const std::string& directory={});
    // A failed read is retried once the native stage changes; ready names stay cached.
    void StageChanged(unsigned stage) {
        if(stage&&stage!=last_stage){last_stage=stage;if(!ready)attempted=false;}
    }
    const char* Get(unsigned id) const {return id<names.size()?names[id].c_str():"";}
    bool Ready() const {return ready;}
private:
    unsigned last_stage=0;
    bool attempted=false,ready=false;
    std::array<std::string,272> names{};
};
}
