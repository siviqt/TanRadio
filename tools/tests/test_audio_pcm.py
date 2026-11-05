#!/usr/bin/env python3
"""Check supported PCM formats and exact mono/stereo frame conversion."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
CHECK = r'''
#include <assert.h>
#include <string.h>
#include <stdio.h>
#include "audio/pcm.h"
int main(void) {
    assert(audio_pcm_supported(44100,16,1));
    assert(audio_pcm_supported(48000,16,2));
    assert(!audio_pcm_supported(32000,16,2));
    assert(!audio_pcm_supported(44100,24,2));
    assert(!audio_pcm_supported(44100,16,3));
    int16_t mono[] = {-32768,-1,0,1,32767};
    int16_t stereo[10] = {0};
    assert(audio_pcm_stereo(mono,sizeof(mono),1,stereo,5) == 5);
    for (unsigned i=0;i<5;i++) assert(stereo[2*i] == mono[i] && stereo[2*i+1] == mono[i]);
    int16_t copy[10];
    assert(audio_pcm_stereo(stereo,sizeof(stereo),2,copy,5) == 5);
    assert(!memcmp(copy,stereo,sizeof(copy)));
    assert(!audio_pcm_stereo(mono,sizeof(mono)-1,1,copy,5));
    assert(!audio_pcm_stereo(stereo,sizeof(stereo)-2,2,copy,5));
    assert(!audio_pcm_stereo(mono,sizeof(mono),1,copy,4));
    assert(!audio_pcm_stereo(NULL,2,1,copy,5));
    puts("PASS: rate/channel validation, exact signed mono duplication, stereo copy and partial-frame rejection");
}
'''
with tempfile.TemporaryDirectory(prefix='tanradio-pcm-') as directory:
    source = Path(directory) / 'pcm.c'
    source.write_text(CHECK)
    binary = Path(directory) / 'pcm'
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-I', str(ROOT / 'main'), str(source),
                    str(ROOT / 'main/audio/pcm.c'), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
