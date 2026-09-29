.section .rdata,"dr"

.p2align 4
.globl _binary_turing_shaders_xz_start
_binary_turing_shaders_xz_start:
.incbin "src/nvidia-modeset/src/shaders/g_turing_shaders"
.globl _binary_turing_shaders_xz_end
_binary_turing_shaders_xz_end:

.p2align 4
.globl _binary_ampere_shaders_xz_start
_binary_ampere_shaders_xz_start:
.incbin "src/nvidia-modeset/src/shaders/g_ampere_shaders"
.globl _binary_ampere_shaders_xz_end
_binary_ampere_shaders_xz_end:

.p2align 4
.globl _binary_hopper_shaders_xz_start
_binary_hopper_shaders_xz_start:
.incbin "src/nvidia-modeset/src/shaders/g_hopper_shaders"
.globl _binary_hopper_shaders_xz_end
_binary_hopper_shaders_xz_end:

.p2align 4
.globl _binary_blackwell_shaders_xz_start
_binary_blackwell_shaders_xz_start:
.incbin "src/nvidia-modeset/src/shaders/g_blackwell_shaders"
.globl _binary_blackwell_shaders_xz_end
_binary_blackwell_shaders_xz_end:

.p2align 4
.globl _binary_gb20x_shaders_xz_start
_binary_gb20x_shaders_xz_start:
.incbin "src/nvidia-modeset/src/shaders/g_gb20x_shaders"
.globl _binary_gb20x_shaders_xz_end
_binary_gb20x_shaders_xz_end:
