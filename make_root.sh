if [ -f bash-static_4.4.18-3_riscv64.deb ]; then echo downloaded; else wget http://cdn-fastly.deb.debian.org/debian-ports/pool-riscv64/main/b/bash/bash-static_4.4.18-3_riscv64.deb; fi
ar x bash-static_4.4.18-3_riscv64.deb
if [ -d ramfs ]; then rm -fr ramfs; fi
mkdir ramfs
cd ramfs
tar xJf ../data.tar.xz
ln -s bin/bash-static init
rm ../data.tar.xz ../control.tar.xz ../debian-binary
mkdir -p bin etc dev home lib proc sbin sys tmp usr mnt nfs root usr/bin usr/lib usr/sbin
echo "\
        mknod dev/null c 1 3 && \
        mknod dev/tty c 5 0 && \
        mknod dev/zero c 1 5 && \
        mknod dev/console c 5 1 && \
        mknod dev/mmcblk0 b 179 0 && \
        mknod dev/mmcblk0p1 b 179 1 && \
        mknod dev/mmcblk0p2 b 179 2 && \
        find . | cpio -H newc -o > ../initramfs.cpio\
        " | fakeroot 
cd ..
scripts/dtc/dtc arch/riscv/kernel/lowrisc.dts -O dtb -o arch/riscv/kernel/lowrisc.dtb
rm -f arch/riscv/kernel/head.o
make ARCH=riscv -j 4
rm -f ../rocket-chip/riscv-tools/riscv-pk/build/payload.o
make -C ../rocket-chip/riscv-tools/riscv-pk/build
riscv64-unknown-elf-objdump -d -S -l vmlinux >vmlinux.dis
riscv64-unknown-elf-objdump -d -l -S ../rocket-chip/riscv-tools/riscv-pk/build/bbl >../rocket-chip/riscv-tools/riscv-pk/build/bbl.dis
