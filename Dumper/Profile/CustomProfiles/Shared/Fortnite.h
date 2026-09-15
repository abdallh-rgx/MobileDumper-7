#pragma once

#include "../../IProfile.h"

class FortniteProfile : public IProfile
{
public:
    FortniteProfile() = default;

    std::vector<std::string> GetSupportedGames() const override
    {
        return {
            "com.epicgames.fortnite",
            "com.epicgames.fn"
        };
    }

    uintptr_t GetGObjects() const override
    {
        constexpr uintptr_t kGObjectsOffset = 0x0DCD6E08;
        return GMemory->GetUnrealModule().OffsetToAddress(kGObjectsOffset);
    }

    uintptr_t GetGNames() const override
    {
        constexpr uintptr_t kGNamesOffset = 0x0DC960C0;
        return GMemory->GetUnrealModule().OffsetToAddress(kGNamesOffset);
    }

    void OverrideInSKOffsets(FInSDKOffsets& Offsets) const override
    {
        Offsets.Statics.GWorld  = 0x0DE38568;
        Offsets.Statics.GEngine = 0x0DE34970;
    }

    void DecryptUTF8(char* Data, int32_t Len) const override
    {
        if (!Data || Len == 0)
            return;
    }

    void DecryptUTF16(char16_t* Data, int32_t Len) const override
    {
        if (!Data || Len == 0)
            return;
    }
};
