# home/

Files placed here are automatically synced into the SystrixOS root filesystem at boot.

```bash
make synchome    # uploads all files in home/ to the FAT32 partition
make run         # boot and see them at / in the shell
```

Notes:
- Filenames are **auto-uppercased** to 8.3 format (`readme.md` → `README.MD`)
- The FAT32 root is wiped first so it always exactly mirrors this folder
- Use `ls` in the shell to see what's on the disk
