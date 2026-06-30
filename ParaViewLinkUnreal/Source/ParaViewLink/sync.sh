import os, sys
from pathlib import Path
from glob import glob
from shutil import move

for i in glob("*.h") + glob("*.cpp"):
  path = Path(i)
  if not path.is_symlink():
    print(f"{i} is not a symlink")
    target = Path(f"/Users/gda/Documents/Claude/Projects/UE and PV/{i}")
    if target.is_file():
        print("Its in Claude")
        path.unlink(missing_ok=True)
        path.symlink_to(target)
    else:
        print("Its NOT in Claude")
