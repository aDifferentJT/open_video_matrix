use crate::srt_c;

fn get_last_error() -> String {
    unsafe {
        core::ffi::CStr::from_ptr(srt_c::srt_getlasterror_str())
            .to_str()
            .unwrap()
            .to_string()
    }
}

pub fn startup() {
    unsafe {
        srt_c::srt_startup();
    }
}

pub struct SrtSocket {
    raw: srt_c::SRTSOCKET,
}

impl SrtSocket {
    pub fn new() -> Result<SrtSocket, String> {
        unsafe {
            let raw = srt_c::srt_create_socket();

            let blocking = true;
            if srt_c::SRT_ERROR
                == srt_c::srt_setsockflag(
                    raw,
                    srt_c::SRT_SOCKOPT_SRTO_RCVSYN,
                    std::mem::transmute::<*const bool, *const core::ffi::c_void>(&blocking),
                    std::mem::size_of_val(&blocking) as core::ffi::c_int,
                )
            {
                return Err(get_last_error());
            }

            Ok(SrtSocket { raw: raw })
        }
    }
}

impl Drop for SrtSocket {
    fn drop(&mut self) {
        unsafe {
            srt_c::srt_close(self.raw);
        }
    }
}

impl SrtSocket {
    pub fn connect(&mut self, name: &str, port: u16) -> Result<(), String> {
        unsafe {
            let name_c = std::ffi::CString::new(name).unwrap();
            let mut addr = srt_c::in_addr { s_addr: 0 };
            match srt_c::inet_aton(name_c.as_ptr(), &mut addr) {
                0 => return Err(format!("Invalid address: {name}")),
                _ => (),
            }
            let name = srt_c::sockaddr_in {
                sin_len: std::mem::size_of::<srt_c::sockaddr_in>() as u8,
                sin_family: srt_c::AF_INET as u8,
                sin_port: port.to_be(),
                sin_addr: addr,
                sin_zero: [0; 8],
            };

            if srt_c::SRT_ERROR
                == srt_c::srt_connect(
                    self.raw,
                    std::mem::transmute::<*const srt_c::sockaddr_in, *const srt_c::sockaddr>(&name),
                    std::mem::size_of_val(&name) as core::ffi::c_int,
                )
            {
                return Err(get_last_error());
            }

            Ok(())
        }
    }

    fn recv_sync(&mut self) -> Result<Vec<i8>, String> {
        unsafe {
            let mut max_size: i32 = 0;
            let mut max_size_len: core::ffi::c_int = 0;
            srt_c::srt_getsockflag(
                self.raw,
                srt_c::SRT_SOCKOPT_SRTO_PAYLOADSIZE,
                std::mem::transmute::<*mut i32, *mut core::ffi::c_void>(&mut max_size),
                &mut max_size_len,
            );
            assert!(max_size_len == std::mem::size_of_val(&max_size) as core::ffi::c_int);

            let mut data = Vec::with_capacity(max_size as usize);
            data.resize(max_size as usize, 0);

            match srt_c::srt_recv(self.raw, data.as_mut_ptr(), max_size) {
                srt_c::SRT_ERROR => return Err(get_last_error()),
                0 => return Err("Socket closed".to_string()),
                size => {
                    data.truncate(size as usize);
                    return Ok(data);
                }
            }
        }
    }

    async fn recv_async(&mut self) -> Result<Vec<i8>, String> {
        unsafe {
            let mut max_size: i32 = 0;
            let mut max_size_len: core::ffi::c_int = 0;
            srt_c::srt_getsockflag(
                self.raw,
                srt_c::SRT_SOCKOPT_SRTO_PAYLOADSIZE,
                std::mem::transmute::<*mut i32, *mut core::ffi::c_void>(&mut max_size),
                &mut max_size_len,
            );
            assert!(max_size_len == std::mem::size_of_val(&max_size) as core::ffi::c_int);

            let mut data = Vec::with_capacity(max_size as usize);
            data.resize(max_size as usize, 0);

            loop {
                match srt_c::srt_recv(self.raw, data.as_mut_ptr(), max_size) {
                    srt_c::SRT_ERROR => {
                        let mut system_errno: core::ffi::c_int = 0;
                        let srt_errno = srt_c::srt_getlasterror(&mut system_errno);

                        if srt_errno == srt_c::SRT_ERRNO_SRT_EASYNCRCV {
                            tokio::task::yield_now().await;
                        } else {
                            return Err(get_last_error());
                        }
                    }
                    0 => return Err("Socket closed".to_string()),
                    size => {
                        data.truncate(size as usize);
                        return Ok(data);
                    }
                }
            }
        }
    }
}

impl std::io::Read for SrtSocket {
    fn read(&mut self, buf: &mut [u8]) -> Result<usize, std::io::Error> {
        unsafe {
            match srt_c::srt_recv(
                self.raw,
                std::mem::transmute::<*mut u8, *mut i8>(buf.as_mut_ptr()),
                buf.len() as core::ffi::c_int,
            ) {
                srt_c::SRT_ERROR => return Err(std::io::Error::other(get_last_error())),
                0 => return Err(std::io::Error::other("Socket closed".to_string())),
                size => {
                    return Ok(size as usize);
                }
            }
        }
    }
}
