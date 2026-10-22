subdir('util')
subdir('airconv')

if not mullion_crossbuild
subdir('nativemetal')
else
subdir('winemetal')
endif

subdir('mullion')
subdir('dxgi')
subdir('d3d11')
subdir('d3d10')

# Direct3D 12 is 64-bit only on Windows, and its descriptor handles take 64 bits
if get_option('enable_d3d12') and cpu_family != 'x86'
subdir('d3d12')
endif

if get_option('enable_nvapi')
subdir('nvapi')
endif

if get_option('enable_nvngx')
subdir('nvngx')
endif