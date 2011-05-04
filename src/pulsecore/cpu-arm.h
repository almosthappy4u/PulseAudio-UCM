#ifndef foocpuarmhfoo
#define foocpuarmhfoo

/***
  This file is part of PulseAudio.

  Copyright 2004-2006 Lennart Poettering
  Copyright 2009 Wim Taymans <wim.taymans@collabora.co.uk> 

  PulseAudio is free software; you can redistribute it and/or modify
  it under the terms of the GNU Lesser General Public License as published
  by the Free Software Foundation; either version 2.1 of the License,
  or (at your option) any later version.

  PulseAudio is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  General Public License for more details.

  You should have received a copy of the GNU Lesser General Public License
  along with PulseAudio; if not, write to the Free Software
  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307
  USA.
***/

#include <stdint.h>

typedef enum pa_cpu_arm_flag {
    PA_CPU_ARM_V6       = (1 << 0),
    PA_CPU_ARM_V7       = (1 << 1),
    PA_CPU_ARM_VFP      = (1 << 2),
    PA_CPU_ARM_EDSP     = (1 << 3),
    PA_CPU_ARM_NEON     = (1 << 4),
    PA_CPU_ARM_VFPV3    = (1 << 5)
} pa_cpu_arm_flag_t;

void pa_cpu_init_arm (void);

/* some optimized functions */
void pa_volume_func_init_arm(pa_cpu_arm_flag_t flags);

#endif /* foocpuarmhfoo */
