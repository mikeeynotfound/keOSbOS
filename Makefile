# Compiler & linker
ASM           = nasm
LIN           = ld
CC            = gcc

# Directory
SOURCE_FOLDER = src
OUTPUT_FOLDER = bin
ISO_NAME      = OS2025
DISK_NAME     = storage
DISK_IMAGE    = $(OUTPUT_FOLDER)/$(DISK_NAME).bin

# Flags
WARNING_CFLAG = -Wall -Wextra -Werror
DEBUG_CFLAG   = -fshort-wchar -g
STRIP_CFLAG   = -nostdlib -fno-stack-protector -nostartfiles -nodefaultlibs -ffreestanding -mno-sse -mno-sse2 -mno-mmx
CFLAGS        = $(DEBUG_CFLAG) $(WARNING_CFLAG) $(STRIP_CFLAG) -m32 -c -I$(SOURCE_FOLDER)
AFLAGS        = -f elf32 -g -F dwarf
LFLAGS        = -T $(SOURCE_FOLDER)/linker.ld -melf_i386


run: all disk
	@qemu-system-i386 -s -S -drive file=$(DISK_IMAGE),format=raw,if=ide,index=0,media=disk -cdrom $(OUTPUT_FOLDER)/$(ISO_NAME).iso
all: build
build: iso
clean:
	rm -rf *.o *.iso $(OUTPUT_FOLDER)/*.o $(OUTPUT_FOLDER)/*.iso $(OUTPUT_FOLDER)/kernel $(OUTPUT_FOLDER)/iso \
		$(OUTPUT_FOLDER)/inserter $(OUTPUT_FOLDER)/shell $(OUTPUT_FOLDER)/clock $(OUTPUT_FOLDER)/hello \
		$(CMD_OUTPUT)

# 4 MiB EXT2 + 4 MiB swap area. An old 4 MiB disk is grown, its files are kept.
DISK_SIZE = 8M

disk: $(DISK_IMAGE)
	@[ $$(stat -c %s $(DISK_IMAGE)) -ge 8388608 ] || qemu-img resize -f raw $(DISK_IMAGE) $(DISK_SIZE) > /dev/null

$(DISK_IMAGE):
	@mkdir -p $(OUTPUT_FOLDER)
	@qemu-img create -f raw $@ $(DISK_SIZE)


inserter:
	@mkdir -p $(OUTPUT_FOLDER)
	@$(CC) -Wno-builtin-declaration-mismatch -g -I$(SOURCE_FOLDER) \
		$(SOURCE_FOLDER)/stdlib/string.c \
		$(SOURCE_FOLDER)/filesystem/ext2.c \
		$(SOURCE_FOLDER)/external/external-inserter.c \
		-o $(OUTPUT_FOLDER)/inserter

user-shell:
	@mkdir -p $(OUTPUT_FOLDER)
	@$(ASM) $(AFLAGS) $(SOURCE_FOLDER)/crt0.s -o crt0.o
	@$(CC) $(CFLAGS) -fno-pie $(SOURCE_FOLDER)/user/user-shell.c -o user-shell.o
	@$(CC) $(CFLAGS) -fno-pie $(SOURCE_FOLDER)/user/syscall.c -o syscall.o
	@$(CC) $(CFLAGS) -fno-pie $(SOURCE_FOLDER)/stdlib/string.c -o string.o
	@$(LIN) -T $(SOURCE_FOLDER)/user-linker.ld -melf_i386 --oformat=binary \
		crt0.o user-shell.o syscall.o string.o -o $(OUTPUT_FOLDER)/shell
	@echo Linking object shell object files and generate flat binary...
	@size --target=binary $(OUTPUT_FOLDER)/shell
	@rm -f *.o

insert-shell: disk inserter user-shell
	@echo Inserting shell into root directory...
	@cd $(OUTPUT_FOLDER); ./inserter -f shell 1 $(DISK_NAME).bin

user-clock:
	@mkdir -p $(OUTPUT_FOLDER)
	@$(ASM) $(AFLAGS) $(SOURCE_FOLDER)/crt0.s -o crt0.o
	@$(CC) $(CFLAGS) -fno-pie $(SOURCE_FOLDER)/user/clock.c -o clock.o
	@$(CC) $(CFLAGS) -fno-pie $(SOURCE_FOLDER)/user/syscall.c -o syscall.o
	@$(LIN) -T $(SOURCE_FOLDER)/user-linker.ld -melf_i386 --oformat=binary \
		crt0.o clock.o syscall.o -o $(OUTPUT_FOLDER)/clock
	@echo Linking clock object files and generate flat binary...
	@size --target=binary $(OUTPUT_FOLDER)/clock
	@rm -f *.o

insert-clock: disk inserter user-clock
	@echo Inserting clock into root directory...
	@cd $(OUTPUT_FOLDER); ./inserter -f clock 1 $(DISK_NAME).bin

user-hello:
	@mkdir -p $(OUTPUT_FOLDER)
	@$(ASM) $(AFLAGS) $(SOURCE_FOLDER)/crt0.s -o crt0.o
	@$(CC) $(CFLAGS) -fno-pie $(SOURCE_FOLDER)/user/hello.c -o hello.o
	@$(CC) $(CFLAGS) -fno-pie $(SOURCE_FOLDER)/user/syscall.c -o syscall.o
	@$(LIN) -T $(SOURCE_FOLDER)/user-linker.ld -melf_i386 --oformat=binary \
		crt0.o hello.o syscall.o -o $(OUTPUT_FOLDER)/hello
	@echo Linking hello object files and generate flat binary...
	@size --target=binary $(OUTPUT_FOLDER)/hello
	@rm -f *.o

insert-hello: disk inserter user-hello
	@echo Inserting hello into root directory...
	@cd $(OUTPUT_FOLDER); ./inserter -f hello 1 $(DISK_NAME).bin

# Bonus: separate shell commands. Every command is its own ELF32 executable
# (book 4.3.6: crt0 + linker.ld in src/user/cmd), inserted into /bin on the disk.
CMD_FOLDER     = $(SOURCE_FOLDER)/user/cmd
CMD_OUTPUT     = $(OUTPUT_FOLDER)/cmd
COMMANDS       = ls mkdir cat cp rm mv find grep memtest
CMD_LIB_OBJS   = $(CMD_OUTPUT)/crt0-cmd.o $(CMD_OUTPUT)/cmdlib.o $(CMD_OUTPUT)/syscall.o $(CMD_OUTPUT)/string.o

user-commands:
	@mkdir -p $(CMD_OUTPUT)
	@$(ASM) $(AFLAGS) $(CMD_FOLDER)/crt0-cmd.s -o $(CMD_OUTPUT)/crt0-cmd.o
	@$(CC) $(CFLAGS) -fno-pie $(CMD_FOLDER)/cmdlib.c -o $(CMD_OUTPUT)/cmdlib.o
	@$(CC) $(CFLAGS) -fno-pie $(SOURCE_FOLDER)/user/syscall.c -o $(CMD_OUTPUT)/syscall.o
	@$(CC) $(CFLAGS) -fno-pie $(SOURCE_FOLDER)/stdlib/string.c -o $(CMD_OUTPUT)/string.o
	@for cmd in $(COMMANDS); do \
		$(CC) $(CFLAGS) -fno-pie $(CMD_FOLDER)/$$cmd.c -o $(CMD_OUTPUT)/$$cmd.o || exit 1; \
		$(LIN) -T $(CMD_FOLDER)/linker.ld -melf_i386 $(CMD_LIB_OBJS) $(CMD_OUTPUT)/$$cmd.o \
			-o $(CMD_OUTPUT)/$$cmd || exit 1; \
	done
	@rm -f $(CMD_OUTPUT)/*.o
	@echo Linking separated shell commands as ELF32 executables...
	@size $(addprefix $(CMD_OUTPUT)/,$(COMMANDS))

insert-commands: disk inserter user-commands
	@echo Inserting separated shell commands into /bin...
	@cd $(CMD_OUTPUT); for cmd in $(COMMANDS); do ../inserter -f $$cmd /bin ../$(DISK_NAME).bin || exit 1; done

insert-user: insert-shell insert-clock insert-hello insert-commands

kernel:
	@$(ASM) $(AFLAGS) src/kernel-entrypoint.s -o bin/kernel-entrypoint.o
	@$(CC) $(CFLAGS) $(SOURCE_FOLDER)/kernel.c -o $(OUTPUT_FOLDER)/kernel.o
	@$(CC) $(CFLAGS) $(SOURCE_FOLDER)/cpu/gdt.c -o $(OUTPUT_FOLDER)/gdt.o
	@$(CC) $(CFLAGS) $(SOURCE_FOLDER)/cpu/portio.c -o $(OUTPUT_FOLDER)/portio.o
	@$(CC) $(CFLAGS) $(SOURCE_FOLDER)/cpu/idt.c -o $(OUTPUT_FOLDER)/idt.o
	@$(CC) $(CFLAGS) $(SOURCE_FOLDER)/cpu/interrupt.c -o $(OUTPUT_FOLDER)/interrupt.o
	@$(CC) $(CFLAGS) $(SOURCE_FOLDER)/text/framebuffer.c -o $(OUTPUT_FOLDER)/framebuffer.o
	@$(CC) $(CFLAGS) $(SOURCE_FOLDER)/driver/keyboard.c -o $(OUTPUT_FOLDER)/keyboard.o
	@$(CC) $(CFLAGS) $(SOURCE_FOLDER)/driver/cmos.c -o $(OUTPUT_FOLDER)/cmos.o
	@$(CC) $(CFLAGS) $(SOURCE_FOLDER)/filesystem/disk.c -o $(OUTPUT_FOLDER)/disk.o
	@$(CC) $(CFLAGS) $(SOURCE_FOLDER)/filesystem/ext2.c -o $(OUTPUT_FOLDER)/ext2.o
	@$(CC) $(CFLAGS) $(SOURCE_FOLDER)/stdlib/string.c -o $(OUTPUT_FOLDER)/string.o
	@$(CC) $(CFLAGS) $(SOURCE_FOLDER)/memory/paging.c -o $(OUTPUT_FOLDER)/paging.o
	@$(CC) $(CFLAGS) $(SOURCE_FOLDER)/memory/swap.c -o $(OUTPUT_FOLDER)/swap.o
	@$(CC) $(CFLAGS) $(SOURCE_FOLDER)/process/process.c -o $(OUTPUT_FOLDER)/process.o
	@$(CC) $(CFLAGS) $(SOURCE_FOLDER)/process/scheduler.c -o $(OUTPUT_FOLDER)/scheduler.o
	@$(CC) $(CFLAGS) $(SOURCE_FOLDER)/process/pipe.c -o $(OUTPUT_FOLDER)/pipe.o
	@$(ASM) $(AFLAGS) $(SOURCE_FOLDER)/process/context-switch.s -o $(OUTPUT_FOLDER)/context-switch.o
	@$(ASM) $(AFLAGS) $(SOURCE_FOLDER)/intsetup.s -o $(OUTPUT_FOLDER)/intsetup.o

	@$(LIN) $(LFLAGS) bin/*.o -o $(OUTPUT_FOLDER)/kernel
	@echo Linking object files and generate elf32...
	@rm -f *.o

iso: kernel
	@mkdir -p $(OUTPUT_FOLDER)/iso/boot/grub
	@cp $(OUTPUT_FOLDER)/kernel        $(OUTPUT_FOLDER)/iso/boot/
	@cp other/grub1                    $(OUTPUT_FOLDER)/iso/boot/grub/
	@cp $(SOURCE_FOLDER)/menu.lst      $(OUTPUT_FOLDER)/iso/boot/grub/
	# TODO: Create ISO image
	@genisoimage -R -b boot/grub/grub1 -no-emul-boot -boot-load-size 4 -A os -input-charset utf8 -boot-info-table -o $(OUTPUT_FOLDER)/$(ISO_NAME).iso $(OUTPUT_FOLDER)/iso
	@rm -r $(OUTPUT_FOLDER)/iso/

.PHONY: run all build clean disk kernel iso inserter user-shell insert-shell user-clock insert-clock user-hello insert-hello insert-user user-commands insert-commands