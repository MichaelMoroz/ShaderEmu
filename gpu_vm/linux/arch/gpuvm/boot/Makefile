# SPDX-License-Identifier: GPL-2.0

OBJCOPYFLAGS_Image := -O binary -R .note -R .note.gnu.build-id -R .comment -S

targets := Image

$(obj)/Image: vmlinux FORCE
	$(call if_changed,objcopy)
