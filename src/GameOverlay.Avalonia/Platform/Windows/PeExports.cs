using System;
using System.Buffers.Binary;
using System.IO;

namespace GameOverlay.Avalonia;

/// <summary>
/// Minimal, read-only PE export-table reader. Given a DLL on disk and an export
/// name, it returns the export's RVA (relative virtual address) without loading
/// the module.
/// </summary>
/// <remarks>
/// This exists for cross-bitness injection: an x64 host cannot
/// <c>LoadLibrary</c> a 32-bit DLL to resolve an export's address the usual way,
/// and it cannot reuse its own <c>kernel32!LoadLibraryW</c> address in a WoW64
/// target because the 32-bit kernel32 sits at a different base. Both problems
/// reduce to "find an export's RVA from the file, then add it to the module's
/// base in the target process" - which is exactly what this returns.
///
/// An export RVA is not rebased when a module loads, so
/// <c>remoteModuleBase + rva</c> is the export's real address in the target
/// regardless of ASLR. The reader parses the file the way it sits on disk, so it
/// converts directory RVAs to file offsets through the section table.
/// </remarks>
internal static class PeExports
{
    /// <summary>
    /// Returns the RVA of <paramref name="exportName"/> in the PE file at
    /// <paramref name="dllPath"/>. Throws if the file is not a valid PE, the
    /// export is absent, or the export is a forwarder (which has no local RVA).
    /// </summary>
    public static uint GetExportRva(string dllPath, string exportName)
    {
        byte[] image = File.ReadAllBytes(dllPath);
        var pe = new ReadOnlySpan<byte>(image);

        // DOS header: 'MZ', with e_lfanew (file offset of the PE header) at 0x3C.
        if (pe.Length < 0x40 || U16(pe, 0) != 0x5A4D)
            throw new BadImageFormatException($"{dllPath} is not a PE image (missing MZ).");

        int peOffset = (int)U32(pe, 0x3C);
        if (peOffset < 0 || peOffset + 24 > pe.Length || U32(pe, peOffset) != 0x0000_4550) // "PE\0\0"
            throw new BadImageFormatException($"{dllPath} has no PE signature.");

        // COFF file header (20 bytes) starts right after the 4-byte signature.
        int coff = peOffset + 4;
        int numberOfSections = U16(pe, coff + 2);
        int sizeOfOptionalHeader = U16(pe, coff + 16);

        int optional = coff + 20;
        ushort magic = U16(pe, optional);
        // The export data directory lives at a different offset in PE32 vs PE32+
        // because the 64-bit optional header has wider fields before the
        // directories. Index 0 of the data directory array is the export table.
        int dataDirOffset = magic == 0x20B ? optional + 112  // PE32+ (x64)
                          : magic == 0x10B ? optional + 96   // PE32  (x86)
                          : throw new BadImageFormatException($"{dllPath} has an unknown optional-header magic 0x{magic:X}.");

        uint exportDirRva = U32(pe, dataDirOffset);
        uint exportDirSize = U32(pe, dataDirOffset + 4);
        if (exportDirRva == 0 || exportDirSize == 0)
            throw new BadImageFormatException($"{dllPath} has no export directory.");

        int sectionTable = optional + sizeOfOptionalHeader;
        int exportDir = RvaToOffset(pe, sectionTable, numberOfSections, exportDirRva);

        // IMAGE_EXPORT_DIRECTORY: parallel arrays of name RVAs, name->ordinal
        // indices, and the export address table (function RVAs).
        uint numberOfNames = U32(pe, exportDir + 24);
        int namesTable = RvaToOffset(pe, sectionTable, numberOfSections, U32(pe, exportDir + 32));
        int ordinalsTable = RvaToOffset(pe, sectionTable, numberOfSections, U32(pe, exportDir + 36));
        int functionsTable = RvaToOffset(pe, sectionTable, numberOfSections, U32(pe, exportDir + 28));

        for (uint i = 0; i < numberOfNames; i++)
        {
            int nameRva = (int)U32(pe, namesTable + (int)i * 4);
            int nameOffset = RvaToOffset(pe, sectionTable, numberOfSections, (uint)nameRva);
            if (!MatchesAscii(pe, nameOffset, exportName)) continue;

            ushort ordinal = U16(pe, ordinalsTable + (int)i * 2);
            uint functionRva = U32(pe, functionsTable + ordinal * 4);

            // A forwarder's "RVA" points inside the export directory at a
            // "Target.dll.Function" string rather than at code, so it has no
            // usable local address. Neither LoadLibraryW nor OverlayDetach is a
            // forwarder; this guards against silently returning a garbage target.
            if (functionRva >= exportDirRva && functionRva < exportDirRva + exportDirSize)
                throw new BadImageFormatException(
                    $"Export '{exportName}' in {dllPath} is a forwarder and has no local address.");

            return functionRva;
        }

        throw new BadImageFormatException($"Export '{exportName}' was not found in {dllPath}.");
    }

    /// <summary>
    /// Translates a directory/table RVA to a file offset using the section
    /// table, since the file is read raw rather than mapped as an image.
    /// </summary>
    private static int RvaToOffset(ReadOnlySpan<byte> pe, int sectionTable, int numberOfSections, uint rva)
    {
        for (int i = 0; i < numberOfSections; i++)
        {
            int section = sectionTable + i * 40;               // IMAGE_SECTION_HEADER is 40 bytes
            uint virtualAddress = U32(pe, section + 12);
            uint virtualSize = U32(pe, section + 8);
            uint rawSize = U32(pe, section + 16);
            uint rawPointer = U32(pe, section + 20);

            // Span with the larger of virtual/raw size: some linkers zero-pad one
            // or the other, and an export string can legitimately sit in either.
            uint span = Math.Max(virtualSize, rawSize);
            if (rva >= virtualAddress && rva < virtualAddress + span)
                return (int)(rawPointer + (rva - virtualAddress));
        }
        throw new BadImageFormatException($"RVA 0x{rva:X} is outside every section.");
    }

    private static bool MatchesAscii(ReadOnlySpan<byte> pe, int offset, string expected)
    {
        for (int i = 0; i < expected.Length; i++)
        {
            if (offset + i >= pe.Length) return false;
            if (pe[offset + i] != (byte)expected[i]) return false;
        }
        // The stored name is NUL-terminated; require the terminator so "Foo"
        // doesn't match "FooBar".
        return offset + expected.Length < pe.Length && pe[offset + expected.Length] == 0;
    }

    private static ushort U16(ReadOnlySpan<byte> pe, int offset)
        => BinaryPrimitives.ReadUInt16LittleEndian(pe.Slice(offset, 2));

    private static uint U32(ReadOnlySpan<byte> pe, int offset)
        => BinaryPrimitives.ReadUInt32LittleEndian(pe.Slice(offset, 4));
}
