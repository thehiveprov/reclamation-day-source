#pragma once

// Just enough JSON for the main menu's overlay screens: writing values, and reading the string fields of a flat object
// (what the overlay sends back, and the worlds' world.json).
namespace MiniJson
{
// A JSON string literal, quotes included
inline std::string Quote(std::string_view aText) noexcept
{
    std::string out = "\"";
    for (const char c : aText)
    {
        switch (c)
        {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (static_cast<unsigned char>(c) < 0x20)
                out += fmt::format("\\u{:04x}", static_cast<unsigned char>(c));
            else
                out += c;
        }
    }
    return out + "\"";
}

// The value of a string field of a flat JSON object ({"key": "value", ...}), empty if absent
inline std::string GetString(std::string_view aJson, std::string_view aKey) noexcept
{
    const std::string pattern = Quote(aKey);
    size_t pos = 0;
    while ((pos = aJson.find(pattern, pos)) != std::string_view::npos)
    {
        pos += pattern.size();
        while (pos < aJson.size() && (aJson[pos] == ' ' || aJson[pos] == '\t' || aJson[pos] == '\n' || aJson[pos] == '\r'))
            ++pos;
        if (pos >= aJson.size() || aJson[pos] != ':')
            continue; // the key text appeared as a value
        ++pos;
        while (pos < aJson.size() && (aJson[pos] == ' ' || aJson[pos] == '\t' || aJson[pos] == '\n' || aJson[pos] == '\r'))
            ++pos;
        if (pos >= aJson.size() || aJson[pos] != '"')
            return {};

        std::string value;
        for (++pos; pos < aJson.size() && aJson[pos] != '"'; ++pos)
        {
            if (aJson[pos] != '\\' || pos + 1 >= aJson.size())
            {
                value += aJson[pos];
                continue;
            }

            switch (const char escaped = aJson[++pos])
            {
            case 'n': value += '\n'; break;
            case 'r': value += '\r'; break;
            case 't': value += '\t'; break;
            case 'u':
                // Only what we write ourselves is read back: \u00XX
                if (pos + 4 < aJson.size())
                {
                    value += static_cast<char>(std::strtoul(std::string(aJson.substr(pos + 1, 4)).c_str(), nullptr, 16));
                    pos += 4;
                }
                break;
            default: value += escaped; break;
            }
        }
        return value;
    }
    return {};
}
} // namespace MiniJson
