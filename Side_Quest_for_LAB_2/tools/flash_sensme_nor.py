#!/usr/bin/env python3
"""
flash_sensme_nor.py
Streams sd_card/sensme.bin (187.6 KB) directly into the W25Q64 SPI NOR Flash
over ST-Link via OpenOCD / GDB using a 2 KB RAM transfer buffer on the STM32F407.
"""

import os
import sys
import time
import subprocess

P_PIO = "C:/Users/Perch/.platformio"
OCD = f"{P_PIO}/packages/tool-openocd/bin/openocd.exe"
GDB = f"{P_PIO}/packages/toolchain-gccarmnoneeabi/bin/arm-none-eabi-gdb.exe"
SCR = f"{P_PIO}/packages/tool-openocd/distro-info/scripts"
ELF = "D:/BCA182_Lab/Side_Quest_for_LAB_2/.pio/build/black_f407zg/firmware.elf"
BIN_PATH = "D:/BCA182_Lab/Side_Quest_for_LAB_2/sd_card/sensme.bin"
RAM_BUF = 0x2000d8c8  # s_sync_buf address in RAM (2048 bytes)

def main():
    if not os.path.exists(BIN_PATH):
        print(f"Error: {BIN_PATH} not found!")
        sys.exit(1)

    with open(BIN_PATH, "rb") as f:
        data = f.read()

    total_len = len(data)
    print(f"Flashing {total_len:,} bytes to W25Q64 NOR Flash via ST-Link...")

    # Start OpenOCD in background
    ocd_proc = subprocess.Popen([
        OCD, "-s", SCR, "-f", "interface/stlink.cfg", "-f", "target/stm32f4x.cfg", "-c", "init"
    ], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    time.sleep(2)

    try:
        # Build GDB commands file
        tmp_gdb = "C:/Users/Perch/AppData/Local/Temp/burn_nor.gdb"
        chunk_dir = "C:/Users/Perch/AppData/Local/Temp/chunks"
        os.makedirs(chunk_dir, exist_ok=True)

        gdb_cmds = [
            "set pagination off",
            "set confirm off",
            "target extended-remote :3333",
            "monitor halt",
            'printf "W25Q64 JEDEC ID: 0x%06X\\n", w25q64_read_jedec_id()',
        ]

        # Break data into 4096-byte sectors for erase, and 2048-byte chunks for programming
        SECTOR_SIZE = 4096
        sectors = (total_len + SECTOR_SIZE - 1) // SECTOR_SIZE

        print(f"Erasing {sectors} sectors (4KB each)...")
        for s in range(sectors):
            addr = s * SECTOR_SIZE
            gdb_cmds.append(f"call w25q64_erase_sector({addr})")

        CHUNK_SIZE = 2048
        chunks_count = (total_len + CHUNK_SIZE - 1) // CHUNK_SIZE
        print(f"Programming {chunks_count} chunks (2KB each)...")

        for c_idx in range(chunks_count):
            start = c_idx * CHUNK_SIZE
            chunk_data = data[start:start + CHUNK_SIZE]
            c_file = os.path.join(chunk_dir, f"c_{c_idx}.bin").replace("\\", "/")
            with open(c_file, "wb") as cf:
                cf.write(chunk_data)

            gdb_cmds.append(f'restore {c_file} binary {RAM_BUF}')
            gdb_cmds.append(f'call w25q64_write({start}, {RAM_BUF}, {len(chunk_data)})')
            if c_idx % 10 == 0 or c_idx == chunks_count - 1:
                gdb_cmds.append(f'printf "Progress: {c_idx+1}/{chunks_count} chunks written\\n"')

        # Re-initialize SensMe and verify
        gdb_cmds.append("call sensme_init()")
        gdb_cmds.append('printf "SensMe Status after flash: ready=%d, songs=%u\\n", sensme_is_ready(), sensme_get_song_count()')
        gdb_cmds.append("monitor resume")
        gdb_cmds.append("detach")
        gdb_cmds.append("quit")

        with open(tmp_gdb, "w", encoding="utf-8") as f:
            f.write("\n".join(gdb_cmds) + "\n")

        print("Executing flash commands over GDB...")
        t0 = time.time()
        res = subprocess.run([GDB, "-batch", "-x", tmp_gdb, ELF],
                             capture_output=True, text=True, timeout=120)
        elapsed = time.time() - t0

        for line in res.stdout.splitlines():
            if any(k in line for k in ("W25Q64", "Progress", "SensMe Status", "error")):
                print(f"  {line}")

        if "ready=1" in res.stdout:
            print(f"SUCCESS: SensMe database flashed to W25Q64 in {elapsed:.1f} seconds!")
        else:
            print(res.stdout[-500:])

    finally:
        ocd_proc.terminate()
        ocd_proc.wait()

if __name__ == "__main__":
    main()
