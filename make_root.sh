if [ -f .config ]; then echo configured; else make ARCH=riscv defconfig; fi
make -s -C ../buildroot
make -s ARCH=riscv -j 4 CROSS_COMPILE=riscv64-unknown-linux-gnu- CONFIG_INITRAMFS_SOURCE="../buildroot/output/images/rootfs.cpio"
mkdir -p ../boom-template/rocket-chip/riscv-tools/riscv-pk/build
( cd ../boom-template/rocket-chip/riscv-tools/riscv-pk/build; ../configure --prefix=$RISCV --host=riscv64-unknown-elf --with-payload=../../../../../riscv-linux/vmlinux --enable-logo --enable-print-device-tree)
rm -f ../boom-template/rocket-chip/riscv-tools/riscv-pk/build/payload.o
make -s -C ../boom-template/rocket-chip/riscv-tools/riscv-pk/build
cp -p ../boom-template/rocket-chip/riscv-tools/riscv-pk/build/bbl ../fpga/board/kc705/boot.bin
riscv64-unknown-elf-strip ../fpga/board/kc705/boot.bin
#riscv64-unknown-elf-objdump -d -S -l vmlinux >vmlinux.dis &
#riscv64-unknown-elf-objdump -d -l -S ../rocket-chip/riscv-tools/riscv-pk/build/bbl >../rocket-chip/riscv-tools/riscv-pk/build/bbl.dis
