"""Build a PS3-compatible ISO from a JB folder using pycdlib."""
import os
import pycdlib

SRC = r"C:\temp\BLUS31473"
DST = r"C:\temp\LEGO_Dimensions_patched.iso"

print("Creating PS3 ISO (UDF 2.50)...")
iso = pycdlib.PyCdlib()
iso.new(udf="2.50", vol_ident="PS3VOLUME")

def add_dir(src_path, iso_path):
    """Recursively add directory contents to ISO."""
    iso.add_directory(iso_path, udf_path=iso_path)
    for entry in os.listdir(src_path):
        full = os.path.join(src_path, entry)
        iso_full = os.path.join(iso_path, entry).replace("\\", "/")
        if os.path.isfile(full):
            size_mb = os.path.getsize(full) / (1024*1024)
            print(f"  Adding: {iso_full} ({size_mb:.1f} MB)")
            iso.add_file(full, iso_full, udf_path=iso_full)
        elif os.path.isdir(full):
            add_dir(full, iso_full)

add_dir(SRC, "/")
print(f"\nWriting ISO to {DST}...")
iso.write(DST)
iso.close()

size_gb = os.path.getsize(DST) / (1024**3)
print(f"Done! ISO created: {size_gb:.2f} GB")
