#pragma once

struct TESObjectREFR;
struct TESForm;

/**
 * A sound file of ours played the way a vanilla sound descriptor (the template) plays its own: with its category,
 * volume and distance falloff. Our descriptor lets the template resolve a sound, then swaps its file for ours.
 *
 * Where the file sits in the engine's resolution is found the first time: by looking for the template's own files,
 * whose resource ids we know (CRC32 with no init/final xor of the lowercase name and folder, as in BA2 archives).
 */
struct FileSound
{
    // acpFile relative to Data, e.g. "Sound\\FX\\ReclamationDay\\SlideMoving.xwm"; the template's file base names
    // (lowercase, no extension) and folder are what the calibration looks for
    FileSound(uint32_t aTemplateFormId, std::vector<std::string> aTemplateFiles, std::string aTemplateFolder, std::string aFile) noexcept;
    ~FileSound() noexcept;

    TP_NOCOPYMOVE(FileSound);

    // Plays the sound following apSource, false when it couldn't
    bool Play(TESObjectREFR* apSource) noexcept;
    // Plays one of the game's own sound descriptors (a BGSSoundDescriptorForm: a weapon's attack sound) following
    // apSource, as Papyrus Sound.Play does; false when it couldn't
    static bool PlayDescriptor(const TESForm* apDescriptor, TESObjectREFR* apSource) noexcept;

    struct ResourceId
    {
        uint32_t file;
        uint32_t ext;
        uint32_t dir;
    };

    static uint32_t Crc(std::string_view aText) noexcept;

private:
    struct Descriptor;

    uint32_t m_templateFormId;
    std::unique_ptr<Descriptor> m_pDescriptor;
};
