use crate::vlc_c;

pub struct Vlc {
    raw: *mut vlc_c::libvlc_instance_t,
}

impl Vlc {
    pub fn new() -> Vlc {
        let raw = unsafe { vlc_c::libvlc_new(0, core::ptr::null()) };

        if raw.is_null() {
            panic!("Could not initialise VLC");
        }

        Vlc { raw: raw }
    }
}

impl Clone for Vlc {
    fn clone(&self) -> Self {
        unsafe {
            vlc_c::libvlc_retain(self.raw);
        }
        Vlc { raw: self.raw }
    }
}

impl Drop for Vlc {
    fn drop(&mut self) {
        unsafe {
            vlc_c::libvlc_release(self.raw);
        }
    }
}

pub struct Media {
    raw: *mut vlc_c::libvlc_media_t,
}

impl Media {
    pub fn from_location(instance: &Vlc, location: &str) -> Media {
        let location_c = std::ffi::CString::new(location).unwrap();
        unsafe {
            Media {
                raw: vlc_c::libvlc_media_new_location(instance.raw, location_c.as_ptr()),
            }
        }
    }

    pub fn from_path(instance: &Vlc, path: &str) -> Media {
        let path_c = std::ffi::CString::new(path).unwrap();
        unsafe {
            Media {
                raw: vlc_c::libvlc_media_new_path(instance.raw, path_c.as_ptr()),
            }
        }
    }
}

impl Clone for Media {
    fn clone(&self) -> Self {
        unsafe {
            vlc_c::libvlc_media_retain(self.raw);
        }
        Media { raw: self.raw }
    }
}

impl Drop for Media {
    fn drop(&mut self) {
        unsafe {
            vlc_c::libvlc_media_release(self.raw);
        }
    }
}

pub struct MediaPlayer {
    raw: *mut vlc_c::libvlc_media_player_t,
}

impl MediaPlayer {
    pub fn new(instance: &Vlc) -> MediaPlayer {
        unsafe {
            MediaPlayer {
                raw: vlc_c::libvlc_media_player_new(instance.raw),
            }
        }
    }

    pub fn from_media(media: &Media) -> MediaPlayer {
        unsafe {
            MediaPlayer {
                raw: vlc_c::libvlc_media_player_new_from_media(media.raw),
            }
        }
    }

    pub fn play(&mut self) {
        unsafe {
            vlc_c::libvlc_media_player_play(self.raw);
        }
    }

    pub fn stop(&mut self) {
        unsafe {
            vlc_c::libvlc_media_player_stop(self.raw);
        }
    }

    pub fn is_playing(&self) -> bool {
        unsafe { vlc_c::libvlc_media_player_is_playing(self.raw) != 0 }
    }
}

impl Clone for MediaPlayer {
    fn clone(&self) -> Self {
        unsafe {
            vlc_c::libvlc_media_player_retain(self.raw);
        }
        MediaPlayer { raw: self.raw }
    }
}

impl Drop for MediaPlayer {
    fn drop(&mut self) {
        unsafe {
            vlc_c::libvlc_media_player_release(self.raw);
        }
    }
}
