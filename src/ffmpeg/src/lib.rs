mod ffmpeg_c;

#[derive(PartialEq, Eq, Clone, Copy)]
pub struct Error {
    raw: i32,
}

const fn MKTAG(a: u8, b: u8, c: u8, d: u8) -> u32 {
    a as u32 | ((b as u32) << 8) | ((c as u32) << 16) | ((d as u32) << 24)
}

const fn FFERRTAG(a: u8, b: u8, c: u8, d: u8) -> Error {
    Error {
        raw: -(MKTAG(a, b, c, d) as i32),
    }
}

impl Error {
    pub const OK: Error = Error { raw: 0 };

    pub const BSF_NOT_FOUND: Error = FFERRTAG(0xF8, b'B', b'S', b'F');
    ///< Bitstream filter not found
    pub const BUG: Error = FFERRTAG(b'B', b'U', b'G', b'!');
    ///< Internal bug, also see AVERROR_BUG2
    pub const BUFFER_TOO_SMALL: Error = FFERRTAG(b'B', b'U', b'F', b'S');
    ///< Buffer too small
    pub const DECODER_NOT_FOUND: Error = FFERRTAG(0xF8, b'D', b'E', b'C');
    ///< Decoder not found
    pub const DEMUXER_NOT_FOUND: Error = FFERRTAG(0xF8, b'D', b'E', b'M');
    ///< Demuxer not found
    pub const ENCODER_NOT_FOUND: Error = FFERRTAG(0xF8, b'E', b'N', b'C');
    ///< Encoder not found
    pub const EOF: Error = FFERRTAG(b'E', b'O', b'F', b' ');
    ///< End of file
    pub const EXIT: Error = FFERRTAG(b'E', b'X', b'I', b'T');
    ///< Immediate exit was requested; the called function should not be restarted
    pub const EXTERNAL: Error = FFERRTAG(b'E', b'X', b'T', b' ');
    ///< Generic error in an external library
    pub const FILTER_NOT_FOUND: Error = FFERRTAG(0xF8, b'F', b'I', b'L');
    ///< Filter not found
    pub const INVALIDDATA: Error = FFERRTAG(b'I', b'N', b'D', b'A');
    ///< Invalid data found when processing input
    pub const MUXER_NOT_FOUND: Error = FFERRTAG(0xF8, b'M', b'U', b'X');
    ///< Muxer not found
    pub const OPTION_NOT_FOUND: Error = FFERRTAG(0xF8, b'O', b'P', b'T');
    ///< Option not found
    pub const PATCHWELCOME: Error = FFERRTAG(b'P', b'A', b'W', b'E');
    ///< Not yet implemented in FFmpeg, patches welcome
    pub const PROTOCOL_NOT_FOUND: Error = FFERRTAG(0xF8, b'P', b'R', b'O');
    ///< Protocol not found
    ///
    pub const STREAM_NOT_FOUND: Error = FFERRTAG(0xF8, b'S', b'T', b'R');
    ///< Stream not found
    /**
     * This is semantically identical to AVERROR_BUG
     * it has been introduced in Libav after our AVERROR_BUG and with a modified value.
     */
    pub const BUG2: Error = FFERRTAG(b'B', b'U', b'G', b' ');
    pub const UNKNOWN: Error = FFERRTAG(b'U', b'N', b'K', b'N');
    ///< Unknown error, typically from an external library
    pub const EXPERIMENTAL: Error = Error { raw: -0x2bb2afa8 };
    ///< Requested feature is flagged experimental. Set strict_std_compliance if you really want to use it.
    pub const INPUT_CHANGED: Error = Error { raw: -0x636e6701 };
    ///< Input changed between calls. Reconfiguration is required. (can be OR-ed with AVERROR_OUTPUT_CHANGED)
    pub const OUTPUT_CHANGED: Error = Error { raw: -0x636e6702 };
    ///< Output changed between calls. Reconfiguration is required. (can be OR-ed with AVERROR_INPUT_CHANGED)
    /* HTTP & RTSP errors */
    pub const HTTP_BAD_REQUEST: Error = FFERRTAG(0xF8, b'4', b'0', b'0');
    pub const HTTP_UNAUTHORIZED: Error = FFERRTAG(0xF8, b'4', b'0', b'1');
    pub const HTTP_FORBIDDEN: Error = FFERRTAG(0xF8, b'4', b'0', b'3');
    pub const HTTP_NOT_FOUND: Error = FFERRTAG(0xF8, b'4', b'0', b'4');
    pub const HTTP_OTHER_4XX: Error = FFERRTAG(0xF8, b'4', b'X', b'X');
    pub const HTTP_SERVER_ERROR: Error = FFERRTAG(0xF8, b'5', b'X', b'X');

    /* POSIX errors */
    pub const EAGAIN: Error = Error {
        raw: -(ffmpeg_c::EAGAIN as i32),
    };
}

impl core::fmt::Display for Error {
    fn fmt(&self, f: &mut core::fmt::Formatter) -> core::fmt::Result {
        let mut msg: [u8; 128] = [0; 128];
        unsafe { ffmpeg_c::av_strerror(self.raw, msg.as_mut_ptr() as *mut i8, msg.len()) };
        write!(
            f,
            "{}",
            core::ffi::CStr::from_bytes_until_nul(&msg[..])
                .unwrap()
                .to_str()
                .unwrap()
        )
    }
}

impl core::fmt::Debug for Error {
    fn fmt(&self, f: &mut core::fmt::Formatter) -> core::fmt::Result {
        write!(f, "{}", self)
    }
}

fn catch_error(status: core::ffi::c_int) -> Result<core::ffi::c_int, Error> {
    if status < 0 {
        Err(Error { raw: status })
    } else {
        Ok(status)
    }
}

pub struct Packet {
    raw: *mut ffmpeg_c::AVPacket,
}

impl Packet {
    fn new() -> Packet {
        unsafe {
            Packet {
                raw: ffmpeg_c::av_packet_alloc(),
            }
        }
    }

    pub fn stream_index(&self) -> core::ffi::c_int {
        unsafe { (*self.raw).stream_index }
    }
}

impl Drop for Packet {
    fn drop(&mut self) {
        unsafe {
            ffmpeg_c::av_packet_free(&mut self.raw);
        }
    }
}

pub struct Stream {
    raw: *mut ffmpeg_c::AVStream,
}

impl Stream {
    pub fn stream_index(&self) -> core::ffi::c_int {
        unsafe { (*self.raw).index }
    }

    pub fn codec_params<'a>(&'a self) -> &'a ffmpeg_c::AVCodecParameters {
        unsafe { (*self.raw).codecpar.as_ref().unwrap() }
    }

    pub fn width(&self) -> core::ffi::c_int {
        self.codec_params().width
    }

    pub fn height(&self) -> core::ffi::c_int {
        self.codec_params().height
    }

    pub fn pixel_format(&self) -> ffmpeg_c::AVPixelFormat {
        self.codec_params().format
    }

    pub fn channel_layout(&self) -> ffmpeg_c::AVChannelLayout {
        self.codec_params().ch_layout
    }
    pub fn sample_format(&self) -> ffmpeg_c::AVSampleFormat {
        self.codec_params().format
    }
    pub fn sample_rate(&self) -> i32 {
        self.codec_params().sample_rate
    }
}

struct DemuxerIO<T> {
    stream: Box<T>,
    context: *mut ffmpeg_c::AVIOContext,
}

impl<T: std::io::Read> DemuxerIO<T> {
    extern "C" fn read_c(
        opaque: *mut core::ffi::c_void,
        buf: *mut u8,
        buf_size: core::ffi::c_int,
    ) -> core::ffi::c_int {
        unsafe {
            let stream = core::mem::transmute::<*mut core::ffi::c_void, &mut T>(opaque);
            let buf = core::slice::from_raw_parts_mut(buf, buf_size as usize);
            match stream.read(buf) {
                Ok(bytes_read) => bytes_read as core::ffi::c_int,
                Err(_) => Error::EOF.raw,
            }
        }
    }

    fn from_read_stream(stream: T) -> DemuxerIO<T> {
        unsafe {
            let mut stream = Box::new(stream);

            let buffer_len = 4096;
            let buffer = ffmpeg_c::av_malloc(4096) as *mut u8;
            assert!(!buffer.is_null());

            let context = ffmpeg_c::avio_alloc_context(
                buffer,
                buffer_len,
                0,
                core::mem::transmute::<&mut T, *mut core::ffi::c_void>(&mut *stream),
                Some(DemuxerIO::<T>::read_c),
                None,
                None,
            );
            assert!(!context.is_null());

            DemuxerIO {
                stream: stream,
                context: context,
            }
        }
    }
}

impl<T> Drop for DemuxerIO<T> {
    fn drop(&mut self) {
        unsafe {
            ffmpeg_c::av_free(self.context as *mut core::ffi::c_void);
            ffmpeg_c::av_free((*self.context).buffer as *mut core::ffi::c_void);
        }
    }
}

pub struct Demuxer<T> {
    context: *mut ffmpeg_c::AVFormatContext,
    io: Option<DemuxerIO<T>>,
}

pub enum MediaType {
    Video,
    Audio,
}

impl Demuxer<()> {
    pub fn open_file(path: &str) -> Result<Demuxer<()>, Error> {
        unsafe {
            let path_c = std::ffi::CString::new(path).unwrap();
            let mut context = core::ptr::null_mut();
            catch_error(ffmpeg_c::avformat_open_input(
                &mut context,
                path_c.as_ptr(),
                core::ptr::null(),
                core::ptr::null_mut(),
            ))?;
            catch_error(ffmpeg_c::avformat_find_stream_info(
                context,
                core::ptr::null_mut(),
            ))?;
            Ok(Demuxer {
                context: context,
                io: None,
            })
        }
    }
}

impl<T: std::io::Read> Demuxer<T> {
    pub fn from_read_stream(stream: T) -> Result<Demuxer<T>, Error> {
        unsafe {
            let io_context = DemuxerIO::from_read_stream(stream);

            let mut context = ffmpeg_c::avformat_alloc_context();
            (*context).pb = io_context.context;
            catch_error(ffmpeg_c::avformat_open_input(
                &mut context,
                b"\0" as *const u8 as *const i8,
                core::ptr::null(),
                core::ptr::null_mut(),
            ))?;
            catch_error(ffmpeg_c::avformat_find_stream_info(
                context,
                core::ptr::null_mut(),
            ))?;
            Ok(Demuxer {
                context: context,
                io: Some(io_context),
            })
        }
    }
}

impl<T> Demuxer<T> {
    fn streams<'a>(&'a self) -> &'a [*mut ffmpeg_c::AVStream] {
        unsafe {
            core::slice::from_raw_parts(
                (*self.context).streams,
                (*self.context).nb_streams as usize,
            )
        }
    }

    pub fn find_best_stream(
        &self,
        stream_type: MediaType,
        related_stream: core::ffi::c_int,
    ) -> Result<(core::ffi::c_int, Stream, Decoder), Error> {
        unsafe {
            let mut decoder_codec = core::ptr::null();
            let stream_index = catch_error(ffmpeg_c::av_find_best_stream(
                self.context,
                match stream_type {
                    MediaType::Video => ffmpeg_c::AVMediaType_AVMEDIA_TYPE_VIDEO,
                    MediaType::Audio => ffmpeg_c::AVMediaType_AVMEDIA_TYPE_AUDIO,
                },
                -1,
                related_stream,
                &mut decoder_codec,
                0,
            ))?;
            let stream = Stream {
                raw: self.streams()[stream_index as usize],
            };
            let decoder = Decoder::new(&stream, decoder_codec)?;
            Ok((stream_index, stream, decoder))
        }
    }

    pub fn read(&mut self) -> Result<Option<Packet>, Error> {
        unsafe {
            let packet = Packet::new();
            match catch_error(ffmpeg_c::av_read_frame(self.context, packet.raw)) {
                Ok(_) => Ok(Some(packet)),
                Err(Error::EOF) => Ok(None),
                Err(e) => Err(e),
            }
        }
    }
}

impl<T> Drop for Demuxer<T> {
    fn drop(&mut self) {
        unsafe {
            ffmpeg_c::avformat_close_input(&mut self.context);
        }
    }
}

pub struct Frame {
    raw: *mut ffmpeg_c::AVFrame,
}

impl Frame {
    fn new() -> Frame {
        unsafe {
            Frame {
                raw: ffmpeg_c::av_frame_alloc(),
            }
        }
    }

    pub fn data<'a>(&'a self) -> &'a [u8] {
        unsafe {
            core::slice::from_raw_parts(
                (*self.raw).data[0],
                ((*self.raw).linesize[0] * (*self.raw).height) as usize,
            )
        }
    }
}

impl Drop for Frame {
    fn drop(&mut self) {
        unsafe {
            ffmpeg_c::av_frame_free(&mut self.raw);
        }
    }
}

pub struct Decoder {
    context: *mut ffmpeg_c::AVCodecContext,
}

impl Decoder {
    fn new(stream: &Stream, codec: *const ffmpeg_c::AVCodec) -> Result<Decoder, Error> {
        unsafe {
            let context = ffmpeg_c::avcodec_alloc_context3(codec);
            catch_error(ffmpeg_c::avcodec_parameters_to_context(
                context,
                stream.codec_params(),
            ))?;
            catch_error(ffmpeg_c::avcodec_open2(
                context,
                codec,
                core::ptr::null_mut(),
            ))?;
            Ok(Decoder { context: context })
        }
    }

    pub fn send(&mut self, packet: &Packet) -> Result<(), Error> {
        unsafe { catch_error(ffmpeg_c::avcodec_send_packet(self.context, packet.raw))? };
        Ok(())
    }

    pub fn receive(&mut self) -> Result<Option<Frame>, Error> {
        let frame = Frame::new();
        match catch_error(unsafe { ffmpeg_c::avcodec_receive_frame(self.context, frame.raw) }) {
            Ok(_) => Ok(Some(frame)),
            Err(Error::EAGAIN) => Ok(None),
            Err(Error::EOF) => Ok(None),
            Err(e) => Err(e),
        }
    }
}

impl Drop for Decoder {
    fn drop(&mut self) {
        unsafe {
            ffmpeg_c::avcodec_free_context(&mut self.context);
        }
    }
}

pub const AV_PIX_FMT_BGRA: ffmpeg_c::AVPixelFormat = ffmpeg_c::AVPixelFormat_AV_PIX_FMT_BGRA;

const fn AV_CHANNEL_LAYOUT_MASK(
    num_channels: core::ffi::c_int,
    mask: u64,
) -> ffmpeg_c::AVChannelLayout {
    ffmpeg_c::AVChannelLayout {
        order: ffmpeg_c::AVChannelOrder_AV_CHANNEL_ORDER_NATIVE,
        nb_channels: num_channels,
        u: ffmpeg_c::AVChannelLayout__bindgen_ty_1 { mask: mask },
        opaque: core::ptr::null_mut(),
    }
}

const AV_CH_FRONT_LEFT: u64 = 1 << ffmpeg_c::AVChannel_AV_CHAN_FRONT_LEFT;
const AV_CH_FRONT_RIGHT: u64 = 1 << ffmpeg_c::AVChannel_AV_CHAN_FRONT_RIGHT;
const AV_CH_LAYOUT_STEREO: u64 = AV_CH_FRONT_LEFT | AV_CH_FRONT_RIGHT;

pub const AV_CHANNEL_LAYOUT_STEREO: ffmpeg_c::AVChannelLayout =
    AV_CHANNEL_LAYOUT_MASK(2, AV_CH_LAYOUT_STEREO);

pub const AV_SAMPLE_FMT_S32: ffmpeg_c::AVSampleFormat = ffmpeg_c::AVSampleFormat_AV_SAMPLE_FMT_S32;

pub struct Scaler {
    context: *mut ffmpeg_c::SwsContext,
}

impl Scaler {
    pub fn new(
        src_width: i32,
        src_height: i32,
        src_pixel_format: ffmpeg_c::AVPixelFormat,
        dst_width: i32,
        dst_height: i32,
        dst_pixel_format: ffmpeg_c::AVPixelFormat,
    ) -> Scaler {
        Scaler {
            context: unsafe {
                ffmpeg_c::sws_getContext(
                    src_width,
                    src_height,
                    src_pixel_format,
                    dst_width,
                    dst_height,
                    dst_pixel_format,
                    0,
                    core::ptr::null_mut(),
                    core::ptr::null_mut(),
                    core::ptr::null(),
                )
            },
        }
    }

    pub fn scale(&mut self, src: &Frame) -> Result<Frame, Error> {
        let mut dst = Frame::new();
        catch_error(unsafe { ffmpeg_c::sws_scale_frame(self.context, dst.raw, src.raw) })?;
        Ok(dst)
    }
}

impl Drop for Scaler {
    fn drop(&mut self) {
        unsafe { ffmpeg_c::sws_freeContext(self.context) };
    }
}

pub struct Resampler {
    context: *mut ffmpeg_c::SwrContext,
    dst_num_channels: core::ffi::c_int,
}

impl Resampler {
    pub fn new(
        mut src_channel_layout: ffmpeg_c::AVChannelLayout,
        src_sample_format: i32,
        src_sample_rate: i32,
        mut dst_channel_layout: ffmpeg_c::AVChannelLayout,
        dst_sample_format: i32,
        dst_sample_rate: i32,
    ) -> Result<Resampler, Error> {
        let mut context = core::ptr::null_mut();
        unsafe {
            catch_error(ffmpeg_c::swr_alloc_set_opts2(
                &mut context,
                &mut dst_channel_layout,
                dst_sample_format,
                dst_sample_rate,
                &mut src_channel_layout,
                src_sample_format,
                src_sample_rate,
                0,
                core::ptr::null_mut(),
            ))?;
            catch_error(ffmpeg_c::swr_init(context))?;
        }
        Ok(Resampler {
            context: context,
            dst_num_channels: dst_channel_layout.nb_channels,
        })
    }

    pub fn convert<'dst, T>(
        &mut self,
        dst: &'dst mut [T],
        src: &Frame,
    ) -> Result<&'dst mut [T], Error> {
        let received_samples = catch_error(unsafe {
            ffmpeg_c::swr_convert(
                self.context,
                &mut dst.as_mut_ptr() as *mut *mut T as *mut *mut u8,
                dst.len() as i32 / self.dst_num_channels as i32,
                (*src.raw).extended_data as *mut *const u8,
                (*src.raw).nb_samples,
            )
        })?;
        let (_, extra_dst) = dst.split_at_mut((received_samples * self.dst_num_channels) as usize);
        Ok(extra_dst)
    }
}

impl Drop for Resampler {
    fn drop(&mut self) {
        unsafe { ffmpeg_c::swr_free(&mut self.context) };
    }
}
