"""Add a single DLL import to a PE executable's import table.

Approach: append a new section containing (a) a rebuilt descriptor array that
copies the existing descriptors AS-IS (same RVAs for their FirstThunk /
OriginalFirstThunk / Name — existing IATs live in their original sections and
stay untouched) plus (b) our new descriptor with its own ILT / IAT / name /
hint-name entries in the new section. Repoint the Import Data Directory to
the new descriptor array. Original code paths that go through the existing
IATs keep working because those IATs are unchanged.

Usage:
  python add_import.py <exe> <dll_name> <export_name>
"""
import pefile
import struct
import sys


def main():
    exe, dll, sym = sys.argv[1], sys.argv[2], sys.argv[3]
    pe = pefile.PE(exe)

    sec_align = pe.OPTIONAL_HEADER.SectionAlignment
    file_align = pe.OPTIONAL_HEADER.FileAlignment
    num_sections = pe.FILE_HEADER.NumberOfSections

    existing = list(pe.DIRECTORY_ENTRY_IMPORT)
    print(f"existing imports: {[e.dll.decode() for e in existing]}")

    # Compute size of the new section
    n_existing = len(existing)
    descriptors_size = (n_existing + 2) * 20  # existing + ours + null

    dll_name_bytes = dll.encode("ascii") + b"\x00"
    sym_entry = b"\x00\x00" + sym.encode("ascii") + b"\x00"
    if len(sym_entry) % 2 == 1:
        sym_entry += b"\x00"

    # Layout:
    #   descriptors ........... descriptors_size
    #   our_ilt (2 × 4 bytes) . 8
    #   our_iat (2 × 4 bytes) . 8
    #   dll_name .............. len(dll_name_bytes)
    #   sym_entry ............. len(sym_entry)
    our_ilt_off = descriptors_size
    our_iat_off = our_ilt_off + 8
    dll_name_off = our_iat_off + 8
    sym_entry_off = dll_name_off + len(dll_name_bytes)
    total = sym_entry_off + len(sym_entry)

    aligned_vsize = ((total + sec_align - 1) // sec_align) * sec_align
    aligned_raw = ((total + file_align - 1) // file_align) * file_align

    last = pe.sections[-1]
    new_rva = ((last.VirtualAddress + last.Misc_VirtualSize + sec_align - 1) //
               sec_align) * sec_align
    new_raw_off = last.PointerToRawData + last.SizeOfRawData

    header_end = pe.DOS_HEADER.e_lfanew + 4 + 20 + pe.FILE_HEADER.SizeOfOptionalHeader
    next_hdr = header_end + num_sections * 40
    first_raw = min(s.PointerToRawData for s in pe.sections if s.PointerToRawData > 0)
    if next_hdr + 40 > first_raw:
        raise SystemExit(
            f"not enough header space (next hdr 0x{next_hdr:x} vs first raw 0x{first_raw:x})")

    data = bytearray(aligned_raw)

    # Descriptors for existing imports — copy RVAs from their original struct.
    for i, e in enumerate(existing):
        struct.pack_into("<IIIII", data, i * 20,
                         e.struct.OriginalFirstThunk,
                         e.struct.TimeDateStamp,
                         e.struct.ForwarderChain,
                         e.struct.Name,
                         e.struct.FirstThunk)

    # Descriptor for our new DLL.
    our_desc_off = n_existing * 20
    struct.pack_into("<IIIII", data, our_desc_off,
                     new_rva + our_ilt_off,     # OriginalFirstThunk
                     0, 0,
                     new_rva + dll_name_off,    # Name
                     new_rva + our_iat_off)     # FirstThunk

    # Null-terminator descriptor already zero.

    # Fill our ILT and IAT (both point to the hint/name entry).
    struct.pack_into("<I", data, our_ilt_off,     new_rva + sym_entry_off)
    struct.pack_into("<I", data, our_ilt_off + 4, 0)  # terminator
    struct.pack_into("<I", data, our_iat_off,     new_rva + sym_entry_off)
    struct.pack_into("<I", data, our_iat_off + 4, 0)

    # DLL name + hint/name entry.
    data[dll_name_off:dll_name_off + len(dll_name_bytes)] = dll_name_bytes
    data[sym_entry_off:sym_entry_off + len(sym_entry)] = sym_entry

    # Patch the file.
    with open(exe, "rb") as f:
        orig = bytearray(f.read())
    if len(orig) < new_raw_off:
        orig.extend(b"\x00" * (new_raw_off - len(orig)))
    orig[new_raw_off:new_raw_off] = bytes(data)

    # NumberOfSections++
    fh_off = pe.DOS_HEADER.e_lfanew + 4
    struct.pack_into("<H", orig, fh_off + 2, num_sections + 1)

    # New section header: .newimp
    new_hdr = struct.pack("<8sIIIIIIHHI",
                          b".newimp\x00",
                          aligned_vsize, new_rva,
                          aligned_raw, new_raw_off,
                          0, 0, 0, 0,
                          0xC0000040)  # INITIALIZED_DATA | READ
    orig[next_hdr:next_hdr + 40] = new_hdr

    # SizeOfImage = new_rva + aligned_vsize
    opt_off = pe.DOS_HEADER.e_lfanew + 4 + 20
    struct.pack_into("<I", orig, opt_off + 56, new_rva + aligned_vsize)

    # Import Data Directory (index 1) at opt_off + 96.
    data_dir = opt_off + 96
    struct.pack_into("<II", orig, data_dir + 1 * 8,
                     new_rva, (n_existing + 2) * 20)

    out = exe + ".patched"
    with open(out, "wb") as f:
        f.write(orig)
    print(f"written: {out} ({len(orig)} bytes, new section @ RVA 0x{new_rva:x})")


main()
