if [ -f .config ]; then echo configured; else make ARCH=riscv defconfig; fi
make -C ../debian-riscv64 cpio
make ARCH=riscv -j 4 CROSS_COMPILE=riscv64-unknown-linux-gnu- CONFIG_INITRAMFS_SOURCE="initramfsnfs.cpio"
mkdir -p ../rocket-chip/riscv-tools/riscv-pk/build
( cd ../rocket-chip/riscv-tools/riscv-pk/build; ../configure --prefix=$RISCV --host=riscv64-unknown-elf --with-payload=../../../../riscv-linux/vmlinux --enable-logo )
scripts/dtc/dtc ../rocket-chip/riscv-tools/riscv-pk/machine/lowrisc.dts -O dtb -o ../rocket-chip/riscv-tools/riscv-pk/build/lowrisc.dtb
rm -f ../rocket-chip/riscv-tools/riscv-pk/build/payload.o
make -C ../rocket-chip/riscv-tools/riscv-pk/build
cp -p ../rocket-chip/riscv-tools/riscv-pk/build/bbl ../fpga/board/nexys4_ddr/boot_nfs.bin
riscv64-unknown-elf-strip ../fpga/board/nexys4_ddr/boot_nfs.bin
make ARCH=riscv -j 4 CROSS_COMPILE=riscv64-unknown-linux-gnu- CONFIG_INITRAMFS_SOURCE="initramfsmmc.cpio"
rm -f ../rocket-chip/riscv-tools/riscv-pk/build/payload.o
make -C ../rocket-chip/riscv-tools/riscv-pk/build
cp -p ../rocket-chip/riscv-tools/riscv-pk/build/bbl ../fpga/board/nexys4_ddr/boot_mmc.bin
riscv64-unknown-elf-strip ../fpga/board/nexys4_ddr/boot_mmc.bin
#riscv64-unknown-elf-objdump -d -S -l vmlinux >vmlinux.dis &
#riscv64-unknown-elf-objdump -d -l -S ../rocket-chip/riscv-tools/riscv-pk/build/bbl >../rocket-chip/riscv-tools/riscv-pk/build/bbl.dis
