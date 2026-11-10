#pragma once

#include <Components/BaseFormComponent.h>

struct BGSKeyword;

struct BGSKeywordForm : BaseFormComponent
{
    virtual ~BGSKeywordForm();

    void* vtbl2;           // 8
    BGSKeyword** keywords; // 10
    uint32_t count;        // 18
    uint32_t pad;

    [[nodiscard]] bool HasKeyword(const void* apKeyword) const noexcept
    {
        for (uint32_t i = 0; keywords && i < count; ++i)
        {
            if (keywords[i] == apKeyword)
                return true;
        }
        return false;
    }
};
