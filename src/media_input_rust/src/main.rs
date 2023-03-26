#![feature(io_error_other)]

use dioxus::prelude::*;
use futures::stream::StreamExt;
use ipc_shared_object::IpcUnmanagedObject;
use triple_buffer::TripleBuffer;

mod srt;
mod srt_c;

mod ffmpeg;
mod ffmpeg_c;

enum GuiToRendererMsg {
    SetColour(String),
}

enum RendererToGuiMsg {
    SetColour(String),
}

struct AppProps {
    from_renderer: flume::Receiver<RendererToGuiMsg>,
    to_renderer: flume::Sender<GuiToRendererMsg>,
}

#[tokio::main]
async fn main() {
    let Ok(listener) = std::net::TcpListener::bind("0.0.0.0:0") else {
        panic!("Could not bind to interface");
    };

    let port = match listener.local_addr().unwrap() {
        std::net::SocketAddr::V4(addr) => addr.port(),
        std::net::SocketAddr::V6(addr) => addr.port(),
    };

    let (to_renderer, from_gui) = flume::unbounded();
    let (to_gui, from_renderer) = flume::unbounded();

    std::thread::spawn(move || {
        tokio::runtime::Runtime::new().unwrap().block_on(async {
            let local_set = tokio::task::LocalSet::new();
            local_set.run_until(renderer(from_gui, to_gui, port)).await;
        });
    });

    let view = dioxus_liveview::LiveViewPool::new();

    let app = axum::Router::new()
        // The root route contains the glue code to connect to the WebSocket
        .route(
            "/",
            axum::routing::get(move || async {
                axum::response::Html(format!(
                    r#"
                <!DOCTYPE html>
                <html>
                <head></head>
                <body> <div id="main"></div> </body>
                {glue}
                </html>
                "#,
                    // TODO this is using an injection attack
                    glue = dioxus_liveview::interpreter_glue(
                        &"\" + `ws://${window.location.host}/ws` + \""
                    )
                ))
            }),
        )
        // The WebSocket route is what Dioxus uses to communicate with the browser
        .route(
            "/ws",
            axum::routing::get(move |ws: axum::extract::WebSocketUpgrade| async move {
                ws.on_upgrade(move |socket| async move {
                    // When the WebSocket is upgraded, launch the LiveView with the app component
                    _ = view
                        .launch_with_props(
                            dioxus_liveview::axum_socket(socket),
                            app,
                            AppProps {
                                from_renderer: from_renderer,
                                to_renderer: to_renderer,
                            },
                        )
                        .await;
                })
            }),
        );

    axum::Server::from_tcp(listener)
        .unwrap()
        .serve(app.into_make_service())
        .await
        .unwrap();
}

fn app(cx: Scope<AppProps>) -> Element {
    let colour = use_state(cx, || "#abcdef".to_string());

    let from_renderer = cx.props.from_renderer.clone();

    use_coroutine(cx, |_: dioxus::prelude::UnboundedReceiver<()>| {
        to_owned![colour];

        async move {
            while let Ok(msg) = from_renderer.recv_async().await {
                match msg {
                    RendererToGuiMsg::SetColour(new_colour) => colour.set(new_colour),
                }
            }
        }
    });

    cx.render(rsx! {
        div {
            "Media Player",
        }
    })
}

async fn renderer(
    from_gui: flume::Receiver<GuiToRendererMsg>,
    to_gui: flume::Sender<RendererToGuiMsg>,
    port: u16,
) {
    let Ok((mut websocket, _)) =
        tokio_tungstenite::connect_async(
            &format!("ws://127.0.0.1:8080/input_{port}")
        ).await
    else {
        panic!("Can't connect to server"); 
    };

    let Some(Ok(tungstenite::protocol::Message::Binary(message))) = websocket.next().await else {
        panic!("no message");
    };
    let Ok(name) = std::str::from_utf8(&message) else { panic!("message not utf8"); };

    // Start thread to listen to the websocket to make sure that ping-pong is handled
    tokio::spawn(async move {
        loop {
            websocket.next().await;
        }
    });

    let mut cpp_output_buffer: IpcUnmanagedObject<triple_buffer::CppTripleBuffer> =
        IpcUnmanagedObject::new(name);
    let mut output_buffer = TripleBuffer::new(cpp_output_buffer.get_mut());

    let mut media =
        MediaStream::open_file("/Users/jonathantanner/Downloads/2021-06-14 20-04-31.mp4").unwrap();
    //let mut media = open_srt("35.178.107.156", 30000).await.unwrap();
    //let mut media = open_srt("127.0.0.1", 30000).await.unwrap();

    let mut ticker = tokio::time::interval(std::time::Duration::from_millis(1000 / 25));
    loop {
        /*
        let Some(frame) = media.get_next_video_frame().unwrap() else { break; };

        let frame = scaler.scale(&frame).unwrap();

        println!("Got frame");

        output_buffer
            .write()
            .video_frame
            .copy_from_slice(frame.planes().first().unwrap().data());
        */

        media.get_next_triple_buffer(output_buffer.write()).unwrap();

        /*
        for x in output_buffer.write().audio_frame {
            if x > 1000000000 {
                println!("{}", x);
            }
        }
        */

        /*
                for i in 0..triple_buffer::AUDIO_SAMPLES_PER_FRAME {
        output_buffer.write().audio_frame[i] = ((i * 5 % triple_buffer::AUDIO_SAMPLES_PER_FRAME) * ((i32::MAX / 100) as usize / triple_buffer::AUDIO_SAMPLES_PER_FRAME)) as i32;
                }
                */

        output_buffer.done_writing();

        let _ = ticker.tick().await;
    }
}

unsafe fn reinterpret_slice<'a, T, U>(data: &'a [T]) -> &'a [U] {
    assert!(data.len() * core::mem::size_of::<T>() % core::mem::size_of::<U>() == 0);
    core::slice::from_raw_parts(
        data.as_ptr() as *const U,
        data.len() * core::mem::size_of::<T>() / core::mem::size_of::<U>(),
    )
}

unsafe fn reinterpret_mut_slice<'a, T, U>(data: &'a mut [T]) -> &'a mut [U] {
    assert!(data.len() * core::mem::size_of::<T>() % core::mem::size_of::<U>() == 0);
    core::slice::from_raw_parts_mut(
        data.as_mut_ptr() as *mut U,
        data.len() * core::mem::size_of::<T>() / core::mem::size_of::<U>(),
    )
}

enum PartialCopyFromSliceResult<'dst, 'src, T> {
    Perfect,
    ExtraDst(&'dst mut [T]),
    ExtraSrc(&'src [T]),
}

trait PartialCopyFromSlice<T> {
    fn partial_copy_from_slice<'dst, 'src>(
        &'dst mut self,
        src: &'src [T],
    ) -> PartialCopyFromSliceResult<'dst, 'src, T>;
}

impl<T: Copy> PartialCopyFromSlice<T> for [T] {
    fn partial_copy_from_slice<'dst, 'src>(
        &'dst mut self,
        src: &'src [T],
    ) -> PartialCopyFromSliceResult<'dst, 'src, T> {
        match std::cmp::Ord::cmp(&self.len(), &src.len()) {
            std::cmp::Ordering::Equal => {
                self.copy_from_slice(src);
                PartialCopyFromSliceResult::Perfect
            }
            std::cmp::Ordering::Greater => {
                let (dst_now, dst_later) = self.split_at_mut(src.len());
                dst_now.copy_from_slice(src);
                PartialCopyFromSliceResult::ExtraDst(dst_later)
            }
            std::cmp::Ordering::Less => {
                let (src_now, src_later) = src.split_at(self.len());
                self.copy_from_slice(src_now);
                PartialCopyFromSliceResult::ExtraSrc(src_later)
            }
        }
    }
}

trait Media {
    fn get_next_video_frame(
        &mut self,
        dst: &mut triple_buffer::VideoFrame,
    ) -> Result<bool, ffmpeg::Error>;
    fn get_next_audio_frame(
        &mut self,
        dst: &mut triple_buffer::AudioFrame,
    ) -> Result<bool, ffmpeg::Error>;
    fn get_next_triple_buffer(
        &mut self,
        dst: &mut triple_buffer::Buffer,
    ) -> Result<bool, ffmpeg::Error>;
}

struct MediaStream {
    /*
    demuxer: ac_ffmpeg::format::demuxer::DemuxerWithStreamInfo<T>,
    video_stream_index: usize,
    video_decoder: ac_ffmpeg::codec::video::VideoDecoder,
    video_scaler: ac_ffmpeg::codec::video::VideoFrameScaler,
    video_queue: std::collections::VecDeque<ac_ffmpeg::codec::video::VideoFrame>,
    audio_stream_index: usize,
    audio_decoder: ac_ffmpeg::codec::audio::AudioDecoder,
    audio_resampler: ac_ffmpeg::codec::audio::AudioResampler,
    audio_queue: std::collections::VecDeque<ac_ffmpeg::codec::audio::AudioFrame>,
    extra_audio_samples: Vec<i32>,
    */
    demuxer: ffmpeg::Demuxer,
    video_stream_index: core::ffi::c_int,
    video_decoder: ffmpeg::Decoder,
    video_scaler: ffmpeg::Scaler,
    video_queue: std::collections::VecDeque<ffmpeg::Frame>,
    audio_stream_index: core::ffi::c_int,
    audio_decoder: ffmpeg::Decoder,
    audio_resampler: ffmpeg::Resampler,
    audio_queue: std::collections::VecDeque<ffmpeg::Frame>,
    extra_audio_samples: Vec<i32>,
}

impl MediaStream {
    fn open_file(path: &str) -> Result<MediaStream, ffmpeg::Error> {
        /*
        let demuxer = ac_ffmpeg::format::demuxer::Demuxer::builder()
            .build(io)?
            .find_stream_info(None)
            .map_err(|(_, err)| err)?;
            */
        let demuxer = ffmpeg::Demuxer::open_file(path)?;

        let (video_stream_index, video_stream, video_decoder) =
            demuxer.find_best_stream(ffmpeg::MediaType::Video, -1)?;

        let (audio_stream_index, audio_stream, audio_decoder) =
            demuxer.find_best_stream(ffmpeg::MediaType::Audio, video_stream_index)?;

        println!("video stream index: {video_stream_index}");
        println!("audio stream index: {audio_stream_index}");

        let video_scaler = ffmpeg::Scaler::new(
            video_stream.width(),
            video_stream.height(),
            video_stream.pixel_format(),
            triple_buffer::WIDTH as i32,
            triple_buffer::HEIGHT as i32,
            ffmpeg::AV_PIX_FMT_BGRA,
        );

        let mut audio_resampler = ffmpeg::Resampler::new(
            audio_stream.channel_layout(),
            audio_stream.sample_format(),
            audio_stream.sample_rate(),
            ffmpeg::AV_CHANNEL_LAYOUT_STEREO,
            ffmpeg::AV_SAMPLE_FMT_S32,
            triple_buffer::SAMPLE_RATE as i32,
        )?;

        /*
        let (video_stream_index, video_stream, video_params) = demuxer
            .streams()
            .iter()
            .map(|stream| (stream, stream.codec_parameters()))
            .enumerate()
            .filter_map(|(index, (stream, params))| {
                Some((index, stream, params.into_video_codec_parameters()?))
            })
            .next()
            .ok_or_else(|| ffmpeg::Error::new("no video stream"))?;

        let video_decoder = ac_ffmpeg::codec::video::VideoDecoder::from_stream(video_stream)?
            .set_option("pixel_format", "bgra")
            .build()?;

        let mut video_scaler = ac_ffmpeg::codec::video::scaler::VideoFrameScaler::builder()
            .source_pixel_format(video_params.pixel_format())
            .source_width(video_params.width())
            .source_height(video_params.height())
            .target_pixel_format(
                ac_ffmpeg::codec::video::frame::PixelFormat::from_str("bgra").unwrap(),
            )
            .target_width(triple_buffer::WIDTH)
            .target_height(triple_buffer::HEIGHT)
            .build()?;

        let (audio_stream_index, audio_stream, audio_params) = demuxer
            .streams()
            .iter()
            .map(|stream| (stream, stream.codec_parameters()))
            .enumerate()
            .filter_map(|(index, (stream, params))| {
                Some((index, stream, params.into_audio_codec_parameters()?))
            })
            .next()
            .ok_or_else(|| ffmpeg::Error::new("no audio stream"))?;

        let audio_decoder =
            ac_ffmpeg::codec::audio::AudioDecoder::from_stream(audio_stream)?.build()?;

        eprintln!("Sample Format: {}", audio_params.sample_format().name());
        let mut audio_resampler = ac_ffmpeg::codec::audio::resampler::AudioResampler::builder()
            .source_channel_layout(audio_params.channel_layout().to_owned())
            .source_sample_format(audio_params.sample_format())
            .source_sample_rate(audio_params.sample_rate())
            .target_channel_layout(
                ac_ffmpeg::codec::audio::frame::ChannelLayout::from_channels(
                    triple_buffer::NUM_CHANNELS as u32,
                )
                .unwrap(),
            )
            .target_sample_format(
                ac_ffmpeg::codec::audio::frame::SampleFormat::from_str("s32").unwrap(),
            )
            .target_sample_rate(triple_buffer::SAMPLE_RATE as u32)
            .build()?;
            */

        Ok(MediaStream {
            demuxer: demuxer,
            video_stream_index: video_stream_index,
            video_decoder: video_decoder,
            video_scaler: video_scaler,
            video_queue: std::collections::VecDeque::new(),
            audio_stream_index: audio_stream_index,
            audio_decoder: audio_decoder,
            audio_resampler: audio_resampler,
            audio_queue: std::collections::VecDeque::new(),
            extra_audio_samples: Vec::new(),
        })
    }
}

impl MediaStream {
    fn pump(&mut self) -> Result<bool, ffmpeg::Error> {
        match self.demuxer.read()? {
            Some(packet) => {
                if packet.stream_index() == self.video_stream_index {
                    self.video_decoder.send(&packet)?;
                    while let Some(frame) = self.video_decoder.receive()? {
                        let frame = self.video_scaler.scale(&frame)?;
                        self.video_queue.push_back(frame);
                    }
                } else if packet.stream_index() == self.audio_stream_index {
                    self.audio_decoder.send(&packet)?;
                    while let Some(frame) = self.audio_decoder.receive()? {
                        self.audio_queue.push_back(frame);
                        /*
                        self.audio_resampler.push(frame);
                        while let Some(frame) = self.audio_resampler.take()? {
                            unsafe {
                                for x in reinterpret_slice::<u8, i32>(
                                    frame.planes().first().unwrap().data(),
                                ) {
                                    use std::io::Write;
                                    std::io::stdout().write(::core::slice::from_raw_parts(
                                        (x as *const i32) as *const u8,
                                        ::core::mem::size_of::<i32>(),
                                    ));
                                }
                            }

                            self.audio_queue.push_back(frame);
                        }
                        */
                    }
                }
                Ok(true)
            }
            None => Ok(false),
        }
    }
}

impl Media for MediaStream {
    fn get_next_video_frame(
        &mut self,
        dst: &mut triple_buffer::VideoFrame,
    ) -> Result<bool, ffmpeg::Error> {
        loop {
            match self.video_queue.pop_front() {
                Some(frame) => {
                    dst.copy_from_slice(frame.data());
                    return Ok(true);
                }
                None => {
                    if !self.pump()? {
                        return Ok(false);
                    }
                }
            }
        }
    }

    fn get_next_audio_frame(
        &mut self,
        dst: &mut triple_buffer::AudioFrame,
    ) -> Result<bool, ffmpeg::Error> {
        let mut dst: &mut [i32] = &mut dst[..];

        /*
        {
            let src = std::mem::replace(&mut self.extra_audio_samples, Vec::new());
            match dst.partial_copy_from_slice(&src) {
                PartialCopyFromSliceResult::Perfect => return Ok(true),
                PartialCopyFromSliceResult::ExtraDst(extra_dst) => dst = extra_dst,
                PartialCopyFromSliceResult::ExtraSrc(extra_src) => {
                    self.extra_audio_samples = extra_src.to_vec();
                    return Ok(true);
                }
            }
        }
        */

        while dst.len() > 0 {
            match self.audio_queue.pop_front() {
                Some(frame) => {
                    dst = self.audio_resampler.convert(dst, &frame)?;
                    /*
                    let src_planes = frame.planes();
                    let src = unsafe { reinterpret_slice(src_planes.first().unwrap().data()) };
                    match dst.partial_copy_from_slice(&src) {
                        PartialCopyFromSliceResult::Perfect => return Ok(true),
                        PartialCopyFromSliceResult::ExtraDst(extra_dst) => dst = extra_dst,
                        PartialCopyFromSliceResult::ExtraSrc(extra_src) => {
                            self.extra_audio_samples = extra_src.to_vec();
                            return Ok(true);
                        }
                    }
                    */
                }
                None => {
                    if !self.pump()? {
                        return Ok(false);
                    }
                }
            }
        }
        Ok(true)
    }

    fn get_next_triple_buffer(
        &mut self,
        dst: &mut triple_buffer::Buffer,
    ) -> Result<bool, ffmpeg::Error> {
        Ok(self.get_next_video_frame(&mut dst.video_frame)?
            && self.get_next_audio_frame(&mut dst.audio_frame)?)
    }
}

/*
fn open_file(path: &str) -> Result<MediaStream<std::fs::File>, ffmpeg::Error> {
    let input = std::fs::File::open(path).map_err(|err| {
        ffmpeg::Error::new(format!("unable to open input file {}: {}", path, err))
    })?;
    MediaStream::new(ac_ffmpeg::format::io::IO::from_seekable_read_stream(input))
}
*/

struct ByteChannelReader {
    channel: tokio::sync::mpsc::UnboundedReceiver<bytes::Bytes>,
}

impl std::io::Read for ByteChannelReader {
    fn read(&mut self, buf: &mut [u8]) -> Result<usize, std::io::Error> {
        match self.channel.try_recv() {
            Ok(data) => {
                buf.copy_from_slice(&data);
                Ok(data.len())
            }
            Err(_) => Ok(0),
        }
    }
}

/*
async fn open_srt(name: &str, port: u16) -> Result<MediaStream<srt::SrtSocket>, ffmpeg::Error> {
    let mut srt = srt::SrtSocket::new()
        .map_err(|err| ffmpeg::Error::new(format!("unable to create srt socket: {}", err)))?;
    srt.connect(name, port)
        .map_err(|err| ffmpeg::Error::new(format!("unable to connect srt {}: {}", name, err)))?;

    /*
    let (send, recv) = tokio::sync::mpsc::unbounded_channel();

    tokio::spawn(async move {
        loop {
            let (timestamp, data) = srt.next().await.unwrap().unwrap();
            send.send(data);
        }
    });

    let reader = ByteChannelReader { channel: recv };
    */

    MediaStream::new(ac_ffmpeg::format::io::IO::from_read_stream(srt))
}
*/
