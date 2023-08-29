# Make the special qemu
./configure --target-list="riscv64-softmmu riscv32-softmmu riscv64-linux-user riscv32-linux-user" --enable-debug --disable-slirp --enable-trace-backends=log -extra-cflags="-DDEBUG_SSI_SD"

# Create the system
./marshal -v -d clean br-base.json
./marshal -v -d build br-base.json
./marshal -v -d install -t prototype br-base.json

# Without SD
qemu-system-riscv64 -nographic -bios none -smp 1 -machine ratona -m 128 -kernel ./images/prototype/br-base/br-base-bin-nodisk
qemu-system-riscv64 -nographic -bios none -smp 2 -machine sifive_u -m 128 -kernel ./images/prototype/br-base/br-base-bin-nodisk

# Create the SD (virtually) (https://chipyard.readthedocs.io/en/stable/Prototyping/VCU118.html#setting-up-the-sdcard)
dd if=/dev/zero of=br-base.sd.img bs=1M count=4096
gdisk br-base.sd.img
:o # GPT create
:x # Expert menu
:l # Configure align
:1 # Align with 1 to write to 34th sector
:m # Close expert
:n # Create part
:1 # Part 1
:34 # Start sector
:+1048576 # +512MB
:af0a # Apple APFS
:n # Create partition
:2 # #1
: # Default
: # Default
: af00 # Apple HFS
:w # Write
# This will create the loop nodes to mount the image as a device
lopdev=$(sudo losetup --show --partscan -f br-base.sd.img)
# Need to look to see which loop was assigned. Depending of that:
sudo mkfs.hfs -v "PrototypeData" ${lopdev}p2
sudo dd if=./images/prototype/br-base/br-base-bin-nodisk-flat of=${lopdev}p1 bs=1M
sync
sudo losetup -d ${lopdev}

# Compile standalone sdboot bootloader (inside fpga/src/main/resources/vcu118/sdboot)
make bin PBUS_FREQ=50000000

# With SD
qemu-system-riscv64 -nographic -bios none -kernel ../../fpga/src/main/resources/vcu118/sdboot/build/sdboot.elf -smp 1 -machine ratona -m 2048 -device sd-card,spi=true,drive=mysd -drive id=mysd,if=none,format=raw,file=br-base.sd.img
# debug (add -s -S)
riscv64-unknown-elf-gdb ../../fpga/src/main/resources/vcu118/sdboot/build/sdboot.elf -ex "target extended-remote localhost:1234"
riscv64-unknown-elf-gdb ./images/prototype/br-base/br-base-bin-nodisk -ex "target extended-remote localhost:1234"
# Powerdown from gdb
monitor quit

