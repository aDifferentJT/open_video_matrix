autocxx::include_cpp! {
    #include "../triple_buffer.hpp"
    generate!("triple_buffer")
    generate_pod!("triple_buffer_buffer")
}

use core::pin::Pin;

pub use ffi::triple_buffer as CppTripleBuffer;
pub use ffi::triple_buffer_buffer as Buffer;

impl Unpin for Buffer {}

pub const WIDTH: usize = 1920;
pub const PITCH: usize = WIDTH * 4;
pub const HEIGHT: usize = 1080;
pub const SIZE: usize = PITCH * HEIGHT;

pub const SAMPLE_RATE: usize = 48_000;
pub const FRAME_RATE: usize = 25;
pub const NUM_CHANNELS: usize = 2;
pub const AUDIO_SAMPLES_PER_FRAME: usize = SAMPLE_RATE * NUM_CHANNELS / FRAME_RATE;

pub type VideoFrame = [u8; SIZE];
pub type AudioFrame = [i32; AUDIO_SAMPLES_PER_FRAME];

pub struct TripleBuffer<'a> {
    data: Pin<&'a mut CppTripleBuffer>,
}

impl TripleBuffer<'_> {
    pub fn new(data: Pin<&mut CppTripleBuffer>) -> TripleBuffer {
        TripleBuffer { data: data }
    }

    pub fn novel_to_read(&mut self) -> bool {
        unsafe { self.data.as_mut().novel_to_read() }
    }

    pub fn about_to_read(&mut self) {
        unsafe { self.data.as_mut().about_to_read() }
    }

    pub fn done_writing(&mut self) {
        unsafe { self.data.as_mut().done_writing() }
    }

    pub fn read<'a>(&'a self) -> &'a Buffer {
        unsafe { self.data.read() }
    }

    pub fn write<'a>(&'a mut self) -> &'a mut Buffer {
        unsafe { self.data.as_mut().write().get_mut() }
    }
}
