# keOSbOS

Sistem operasi 32-bit x86 sederhana untuk Tugas Besar IF2130 Sistem Operasi 2026/2027.

## Nama Kelompok

**keOSbOS**

| Nama | NIM |
|---|---|
| Ahmad Humam Isma | 13525032 |
| Christopherus Michael J Tobing | 13525109 |
| Muhammad Fauzi Muharam | 13525091 |
| I Made Ardana | 13525121 |

## Daftar Isi

- [Nama Kelompok](#nama-kelompok)
- [Cara Run](#cara-run)
  - [Kebutuhan](#kebutuhan)
  - [Build dan Jalankan](#build-dan-jalankan)
  - [Memasukkan File ke Disk](#memasukkan-file-ke-disk)
  - [Debug dengan GDB](#debug-dengan-gdb)
  - [Tes di Host](#tes-di-host)
- [Fitur yang Dibuat](#fitur-yang-dibuat)
  - [Milestone 0: Toolchain, Kernel, GDT](#milestone-0-toolchain-kernel-gdt)
  - [Milestone 1: Interrupt, Driver](#milestone-1-interrupt-driver)
  - [Milestone 2: File System EXT2](#milestone-2-file-system-ext2)
  - [Milestone 3: Paging, User Mode, Shell](#milestone-3-paging-user-mode-shell)
  - [Milestone 4: Process, Scheduler, Multitasking](#milestone-4-process-scheduler-multitasking)
  - [Perintah Shell](#perintah-shell)
- [Struktur Repository](#struktur-repository)
- [Maskot Kelompok](#maskot-kelompok)

## Cara Run

### Kebutuhan

Dijalankan di Linux (atau WSL) dengan paket berikut:

- `nasm`
- `gcc` dengan dukungan 32-bit (`gcc-multilib` di Ubuntu/Debian)
- `binutils` (`ld`)
- `genisoimage`
- `qemu-system-i386` dan `qemu-img` (paket `qemu-system-x86` dan `qemu-utils`)

Contoh di Ubuntu/Debian:

```bash
sudo apt install nasm gcc-multilib binutils genisoimage qemu-system-x86 qemu-utils
```

### Build dan Jalankan

```bash
make insert-user    # buat disk, compile dan masukkan shell, clock, serta hello
make iso            # compile kernel dan buat bin/OS2025.iso
qemu-system-i386 -drive file=bin/storage.bin,format=raw,if=ide,index=0,media=disk -cdrom bin/OS2025.iso
```

Setelah boot, shell muncul dengan prompt `keOSbOS:/$`. Ketik `help` untuk daftar perintah.

Kernel memformat disk otomatis kalau `bin/storage.bin` masih kosong. Isi disk tetap tersimpan antar boot. Untuk mulai dari disk kosong, hapus dulu `bin/storage.bin` lalu jalankan `make insert-shell` lagi.

Kalau `shell` tidak ada di disk, kernel menampilkan pesan error lalu berhenti.

### Memasukkan File ke Disk

`bin/inserter` (dibuat oleh `make inserter`) menyalin file dari host ke `storage.bin`:

```bash
cd bin
./inserter [-f] <file> <parent_inode> storage.bin
```

- `parent_inode` adalah inode folder tujuan. Root adalah inode `1`.
- `-f` menimpa file dengan nama yang sama.

Contoh:

```bash
cd bin
printf 'halo dunia\napel merah\n' > notes.txt
./inserter notes.txt 1 storage.bin
```

### Debug dengan GDB

`make run` menjalankan QEMU dengan `-s -S`. Artinya QEMU membuka GDB server di port `1234` dan menunggu dulu sebelum mulai.

Lanjutkan eksekusi dengan salah satu cara:

- Hubungkan GDB (`target remote :1234`, lalu `continue`).
- Ketik `c` di monitor QEMU.

`make run` tidak memasukkan program user ke disk, jadi jalankan `make insert-user` dulu.

### Tes di Host

Driver EXT2 dan shell bisa dites di host tanpa QEMU. Disk diganti array 4 MiB di memori:

```bash
gcc -g -fsanitize=address,undefined -Isrc tests/ext2_read_test.c  -o /tmp/ext2_read_test  && /tmp/ext2_read_test
gcc -g -fsanitize=address,undefined -Isrc tests/ext2_write_test.c -o /tmp/ext2_write_test && /tmp/ext2_write_test

gcc -g -fsanitize=address,undefined -Isrc -Dmain=shell_main -c src/user/user-shell.c -o /tmp/shell.o
gcc -g -fsanitize=address,undefined -Isrc tests/user_shell_test.c /tmp/shell.o -o /tmp/user_shell_test && /tmp/user_shell_test
```

## Fitur yang Dibuat

### Milestone 0: Toolchain, Kernel, GDT

- Kernel di-boot oleh GRUB legacy lewat multiboot, lalu dikemas jadi ISO.
- Global Descriptor Table untuk segmen kernel (code dan data), lalu masuk protected mode.

### Milestone 1: Interrupt, Driver

- Text framebuffer 80x25 dengan warna dan kontrol cursor.
- IDT dan ISR stub untuk vector 0 sampai 63, plus PIC remap ke offset `0x20` dan `0x28`.
- Driver keyboard PS/2 (scancode set 1). Mendukung Shift, Caps Lock, backspace dan tab.

### Milestone 2: File System EXT2

Menggunakan EXT2 – IF2130 Edition di disk ATA 4 MiB.

- Driver disk ATA PIO (`read_blocks` dan `write_blocks`).
- Superblock, block group descriptor, block bitmap, inode bitmap dan inode table.
- Pointer data direct, indirect dan doubly-indirect.
- Alokasi block dan inode memakai First Fit.
- `read`: baca file. Mengembalikan error kalau target folder, buffer kurang, file tidak ada, atau parent tidak valid.
- `read_directory`: baca isi tabel direktori.
- `write`: buat file atau folder. Tabel direktori bertambah block otomatis kalau penuh. Sebelum menulis, driver mengecek dulu sisa ruang, jadi write yang gagal tidak meninggalkan data setengah jadi.
- `delete`: hapus file atau folder kosong, sekaligus membebaskan block dan inode.

### Milestone 3: Paging, User Mode, Shell

- **Paging:** kernel dipindah ke higher half (`0xC0000000`), dengan allocator page frame untuk program user.
- **Segmen user dan TSS:**
  - Segmen code dan data ring 3 ditambahkan di GDT.
  - TSS menyimpan stack kernel yang dipakai saat terjadi interrupt dari user mode.
- **Program user:**
  - Shell di-link sebagai flat binary di alamat virtual `0x0`.
  - Kernel membaca file `shell` dari root disk, memetakan memorinya, lalu masuk ke ring 3 lewat `iret`.
- **System call `int 0x30`:** gate-nya DPL 3 supaya bisa dipanggil dari user mode. Nomor syscall ada di `eax` dan argumennya di `ebx`, `ecx` dan `edx`.

  | No | Syscall | Fungsi |
  |---|---|---|
  | 0 | `read` | baca file |
  | 1 | `read_directory` | baca tabel direktori |
  | 2 | `write` | buat file atau folder |
  | 3 | `delete` | hapus file atau folder kosong |
  | 4 | `getchar` | ambil tombol terakhir dari keyboard |
  | 5 | `putchar` | cetak satu karakter berwarna |
  | 6 | `puts` | cetak string berwarna |
  | 7 | `keyboard_on` | aktifkan input keyboard |
  | 8 | `clear_screen` | bersihkan layar |
  | 9 | `exec` | jalankan executable sebagai process baru |
  | 10 | `ps` | ambil daftar process dan pemakaian memorinya |
  | 11 | `kill` | hentikan process berdasarkan PID |
  | 12 | `exit` | hentikan process yang sedang berjalan |
  | 13 | `get_time` | baca tanggal dan waktu dari CMOS RTC |
  | 14 | `putchar_at` | tulis karakter di posisi tertentu tanpa menggeser cursor |
  | 15 | `yield` | berikan sisa time slice ke process lain |
  | 16 | `get_pid` | ambil PID process yang sedang berjalan |

- **Terminal di kernel:** output syscall mendukung `\n`, `\t`, backspace, pindah baris otomatis dan scroll.

### Milestone 4: Process, Scheduler, Multitasking

- **Process:** setiap program user punya `ProcessControlBlock`, context register, dan page directory sendiri. Shell adalah process 0 (PID 0).
- **Page directory per process:** image, heap, dan stack user dipetakan ke alamat virtual `0x0..0x400000`, sedangkan higher half kernel dipakai bersama.
- **Scheduler:** PIT channel 0 memicu IRQ0 pada 100 Hz. Handler IRQ0 menyimpan context process yang sedang berjalan, lalu `process_context_switch()` melakukan `iret` ke process round robin berikutnya.
- **Context switch assembly:** `src/process/context-switch.s` memuat CR3 baru, mengembalikan register, dan masuk kembali ke ring 3.
- **Multitasking:** `exec`, `ps`, `kill`, `exit`, dan `yield` tersedia lewat syscall. Process dapat menunggu keyboard atau waktu dengan `yield` supaya tidak memonopoli CPU.
- **Program eksternal:** `hello` adalah flat binary terpisah yang dijalankan lewat `exec hello`, bukan hardcode di shell. `clock` menampilkan waktu WIB di sudut kanan bawah.

### Bonus: PATH Persistent

- Perintah yang bukan perintah bawaan shell dicari sebagai program di setiap folder `PATH` (dipisah `:`), berurutan. Contoh: dengan `PATH=/bin:/`, mengetik `clock` menjalankan `/bin/clock` kalau ada, kalau tidak `/clock`.
- `export PATH=/bin:/` mengubah PATH dan langsung menyimpannya ke file `/.path`. Shell membaca file itu saat mulai, jadi PATH tetap sama setelah reboot. `export` tanpa argumen menampilkan PATH.
- Nilai awal `PATH=/`, sehingga `hello` dan `clock` bisa dijalankan tanpa `exec`.
- Perintah bawaan selalu didahulukan. Entry PATH yang kosong atau foldernya tidak ada dilewati.

### Perintah Shell

Path bisa relatif atau absolut, dan bisa memakai `.` serta `..`.

| Perintah | Keterangan |
|---|---|
| `help` | daftar perintah |
| `clear` | bersihkan layar |
| `cd [dir]` | pindah folder, tanpa argumen kembali ke `/` |
| `ls [dir]` | isi folder, folder ditandai `/` |
| `mkdir <dir>` | buat folder |
| `cat <file>` | tampilkan isi file |
| `cp <src> <dst>` | salin file ke nama baru atau ke dalam folder |
| `rm <path>` | hapus file atau folder kosong |
| `mv <src> <dst>` | pindah atau ganti nama file |
| `find <name>` | cari file atau folder dengan nama persis, mulai dari `/` |
| `grep <pola> [file]` | tampilkan baris yang mengandung pola |
| `exec <file>` | jalankan program user sebagai process baru |
| `ps` | tampilkan PID, state, memori, dan nama process |
| `kill <pid>` | hentikan process selain PID 0 |
| `export [PATH=a:b]` | tampilkan atau ubah PATH, disimpan di `/.path` |
| `<program>` | jalankan program yang ditemukan di PATH, sama seperti `exec` |
| `a \| b` | output `a` jadi input `b`, maksimal 8 tahap |

Argumen yang berisi spasi ditulis di dalam tanda kutip, misalnya `cat notes.txt | grep "apel merah"`.

Batasan saat ini:

- `cp` dan `mv` hanya untuk file.
- `rm` hanya untuk file atau folder kosong.
- File kosong tidak bisa disalin, karena `write` dengan ukuran 0 berarti membuat folder.
- Ukuran file yang bisa dibaca shell maksimal 64 KiB.
- Tabel direktori yang bisa dibaca shell maksimal 32 block (16 KiB).

## Struktur Repository

```
src/
├── kernel-entrypoint.s   # entry multiboot, paging awal, load GDT, masuk user mode
├── kernel.c              # kernel_setup, memuat dan menjalankan shell
├── intsetup.s            # ISR stub dan handler interrupt umum
├── crt0.s                # entry point program user
├── linker.ld             # linker script kernel (higher half)
├── user-linker.ld        # linker script program user (flat binary di 0x0)
├── cpu/                  # GDT, IDT, interrupt, syscall, port I/O
├── driver/               # keyboard dan CMOS RTC
├── filesystem/           # disk ATA dan EXT2
├── memory/               # paging dan page directory per process
├── process/              # PCB, scheduler, dan context switch assembly
├── text/                 # framebuffer dan terminal
├── stdlib/               # memset, memcpy, memcmp, memmove
├── user/                 # shell, clock, hello, dan wrapper syscall sisi user
├── external/             # inserter (program host)
└── header/               # header semua modul di atas
tests/                    # tes host untuk EXT2 dan shell
```

## Maskot Kelompok

![Maskot keOSbOS](mascot.jpg)

