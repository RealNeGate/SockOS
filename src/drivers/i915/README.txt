Intel GPU drivers

  For now, We're looking to target the Skylake, Kabylake and Coffeelake iGPUs
  but from our understanding the different GPUs do not change architecture very
  often. Currently this doc is based almost exclusively on:

    https://01.org/linuxgraphics/hardware-specification-prms/2016-intelr-processors-based-kaby-lake-platform

  Contact @RealNeGate if you find problems or need help :)

Documentation

  We really don't like the iGPU docs that Intel provides so I'm going to restructure
  some of the core details.

  Acronyms:
    Logical Render Context Address - LCRA

  Engine:
    These are different components of the GPU capable of different jobs, these are called:
      * 3D      - normal 3D pipeline with all it's nice shaders and fixed-function elements.
      * Media   -
      * Blitter -

  Instruction Ring buffers:
    These act as the queue in the Vulkan sense and serve to submit GPU commands.
    If you need bigger streams you use the Batch buffers (which are the command
    buffers in a sense). They consist of:
      * Head pointer         - aligned to 4kb, can be anywhere mapped in the Global GTT.
      * Tail pointer         - must not be more than 2MB ahead of the head pointer.
      * RING_BUFFER_START    - TODO
      * RING_BUFFER_CONTROL  - TODO

  Ring context:
    This is a 320 byte region (5 cachelines) of state for the ring buffer such as
    page table info with the last cacheline being Engine-specific.

  Instruction Batch buffer:
    stream of instructions starting with `MI_BATCH_BUFFER_START`. They are 8-byte
    aligned in both address and length.

  Important GPU commands:
    We'll be looking over these soon enough :P
      * 3DPRIMITIVE  - this is a draw call
      * GPGPU_WALKER - this is a compute dispatch call
