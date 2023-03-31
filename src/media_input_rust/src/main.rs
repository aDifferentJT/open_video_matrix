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

    /*
    let mut media =
        MediaStream::open_file("/Users/jonathantanner/Downloads/2021-06-14 20-04-31.mp4").unwrap();
        */
    //let mut media = open_srt("35.178.107.156", 30000).await.unwrap();
    let mut media = MediaStream::open_srt("127.0.0.1", 30000).unwrap();

    tokio::time::sleep(core::time::Duration::from_secs(1));

    loop {
        media.get_next_triple_buffer(output_buffer.write()).unwrap();
        output_buffer.done_writing();
        output_buffer.wait_for_sync();
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

struct MediaStream<T> {
    demuxer: ffmpeg::Demuxer<T>,
    video_stream_index: core::ffi::c_int,
    video_decoder: ffmpeg::Decoder,
    video_scaler: ffmpeg::Scaler,
    video_queue: std::collections::VecDeque<ffmpeg::Frame>,
    audio_stream_index: core::ffi::c_int,
    audio_decoder: ffmpeg::Decoder,
    audio_resampler: ffmpeg::Resampler,
    audio_queue: std::collections::VecDeque<ffmpeg::Frame>,
}

impl<T> MediaStream<T> {
    fn new(demuxer: ffmpeg::Demuxer<T>) -> Result<MediaStream<T>, ffmpeg::Error> {
        let (video_stream_index, video_stream, video_decoder) =
            demuxer.find_best_stream(ffmpeg::MediaType::Video, -1)?;

        let (audio_stream_index, audio_stream, audio_decoder) =
            demuxer.find_best_stream(ffmpeg::MediaType::Audio, video_stream_index)?;

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
        })
    }
}

impl MediaStream<()> {
    fn open_file(path: &str) -> Result<MediaStream<()>, ffmpeg::Error> {
        let demuxer = ffmpeg::Demuxer::open_file(path)?;
        MediaStream::new(demuxer)
    }
}

impl MediaStream<srt::SrtSocket> {
    fn open_srt(name: &str, port: u16) -> Result<MediaStream<srt::SrtSocket>, ffmpeg::Error> {
        let mut srt = match srt::SrtSocket::new() {
            Ok(srt) => srt,
            Err(err) => panic!("unable to create srt socket: {}", err),
        };
        match srt.connect(name, port) {
            Ok(_) => {}
            Err(err) => panic!("unable to connect srt {}: {}", name, err),
        }

        let demuxer = ffmpeg::Demuxer::from_read_stream(srt)?;
        MediaStream::new(demuxer)
    }
}

impl<T> MediaStream<T> {
    fn pump(&mut self) -> Result<bool, ffmpeg::Error> {
        match self.demuxer.read()? {
            Some(packet) => {
                if packet.stream_index() == self.video_stream_index {
                    self.video_decoder.send(&packet)?;
                    while let Some(frame) = self.video_decoder.receive()? {
                        self.video_queue.push_back(frame);
                    }
                } else if packet.stream_index() == self.audio_stream_index {
                    self.audio_decoder.send(&packet)?;
                    while let Some(frame) = self.audio_decoder.receive()? {
                        self.audio_queue.push_back(frame);
                    }
                }
                Ok(true)
            }
            None => Ok(false),
        }
    }
}

impl<T> Media for MediaStream<T> {
    fn get_next_video_frame(
        &mut self,
        dst: &mut triple_buffer::VideoFrame,
    ) -> Result<bool, ffmpeg::Error> {
        loop {
            match self.video_queue.pop_front() {
                Some(frame) => {
                    let frame = self.video_scaler.scale(&frame)?;
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

        while dst.len() > 0 {
            match self.audio_queue.pop_front() {
                Some(frame) => {
                    dst = self.audio_resampler.convert(dst, &frame)?;
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
