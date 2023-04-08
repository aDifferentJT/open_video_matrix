#![feature(async_closure)]

use dioxus::prelude::*;
use futures::stream::StreamExt;
use ipc_shared_object::IpcUnmanagedObject;
use tokio::time::Duration;
use triple_buffer::TripleBuffer;
use webrtc::api::media_engine::{MediaEngine, MIME_TYPE_H264};
use webrtc::api::APIBuilder;
use webrtc::peer_connection::configuration::RTCConfiguration;
use webrtc::peer_connection::sdp::session_description::RTCSessionDescription;
use webrtc::peer_connection::RTCPeerConnection;
use webrtc::rtp_transceiver::rtp_codec::RTCRtpCodecCapability;
use webrtc::track::track_local::track_local_static_sample::TrackLocalStaticSample;
use webrtc::track::track_local::TrackLocal;

#[macro_use]
extern crate lazy_static;

lazy_static! {
    static ref api: webrtc::api::API = (|| {
        let mut media_engine = MediaEngine::default();
        media_engine.register_default_codecs();

        // Create the API object with the MediaEngine
        APIBuilder::new().with_media_engine(media_engine).build()
    })();
}

enum GuiToEncoderMsg {
    AddPeerConnection(std::sync::Arc<RTCPeerConnection>),
}

enum EncoderToGuiMsg {
    SetColour(String),
}

struct AppProps {
    from_renderer: flume::Receiver<EncoderToGuiMsg>,
    to_renderer: flume::Sender<GuiToEncoderMsg>,
}

const PLAYER_HTML: &str = r#"
<div id="remoteVideos"></div>

<script>
  async function makeCall() {
    let pc = new RTCPeerConnection()

    pc.ontrack = function (event) {
      var el = document.createElement(event.track.kind)
      el.srcObject = event.streams[0]
      el.autoplay = true
      el.controls = true
    
      document.getElementById('remoteVideos').appendChild(el)
    }

    pc.oniceconnectionstatechange = e => console.log(pc.iceConnectionState)
    pc.onicecandidate = event => console.log(event)

    // Offer to receive 1 audio, and 1 video track
    pc.addTransceiver('video', {'direction': 'sendrecv'})
    pc.addTransceiver('audio', {'direction': 'sendrecv'})

    const offer = await pc.createOffer();
    pc.setLocalDescription(offer);

    const resp = await fetch(
      '/sdp',
      {method: 'post',
      headers: new Headers({'content-type': 'application/json'}),
      body: JSON.stringify(offer),
      },
    )
    const answer = await resp.json();
    console.log(answer)
    const remoteDesc = new RTCSessionDescription(answer);
    await pc.setRemoteDescription(remoteDesc);
  }

  makeCall();
</script>
"#;

#[tokio::main]
async fn main() -> Result<(), anyhow::Error> {
    dcv_color_primitives::initialize();

    let Ok(listener) = std::net::TcpListener::bind("0.0.0.0:60000") else {
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
            local_set.run_until(encoder(from_gui, to_gui, port)).await;
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
                <body>
                  <button
                    onclick="window.parent.postMessage({{msg: 'show_detail_view', data: `http://${{window.location.host}}/player`}}, '*')"
                  >
                    Open Player
                  </button>
                  <div id="main"></div>
                </body>
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
                        .launch(
                            dioxus_liveview::axum_socket(socket),
                            app,
                        )
                        .await;
                })
            }),
        )
        .route(
            "/player",
            axum::routing::get(|| async { axum::response::Html(PLAYER_HTML) }),
        )
        .route(
            "/sdp",
            axum::routing::post(
                async move |axum::Json(offer): axum::Json<RTCSessionDescription>| {
                    // TODO this shouldn't panic

                    println!("remote_handler receive from /sdp");

                    let peer_connection = std::sync::Arc::new(
                        api.new_peer_connection(RTCConfiguration {
                            ..Default::default()
                        })
                        .await.unwrap(),
                    );

                    to_renderer.send(GuiToEncoderMsg::AddPeerConnection(peer_connection.clone()));

                    if let Err(err) = peer_connection.set_remote_description(offer).await {
                        panic!("{}", err);
                    }

                    // Create an answer to send to the other process
                    let answer = match peer_connection.create_answer(None).await {
                        Ok(a) => a,
                        Err(err) => panic!("{}", err),
                    };

                    // Create channel that is blocked until ICE Gathering is complete
                    let mut gather_complete = peer_connection.gathering_complete_promise().await;

                    // Sets the LocalDescription, and starts our UDP listeners
                    if let Err(err) = peer_connection.set_local_description(answer).await {
                        panic!("{}", err);
                    }

                    // Block until ICE Gathering is complete, disabling trickle ICE
                    // we do this because we only can exchange one signaling message Candidate
                    // in a production application you should exchange ICE Candidates via OnICECandidate
                    let _ = gather_complete.recv().await;

                    axum::Json(peer_connection.local_description().await)
                },
            ),
        );

    axum::Server::from_tcp(listener)
        .unwrap()
        .serve(app.into_make_service())
        .await
        .unwrap();

    Ok(())
}

fn app(cx: Scope) -> Element {
    cx.render(rsx! {
        div {
            "Hello world",
        }
    })
}

async fn encoder(
    from_gui: flume::Receiver<GuiToEncoderMsg>,
    to_gui: flume::Sender<EncoderToGuiMsg>,
    port: u16,
) {
    let Ok((mut websocket, _)) =
        tokio_tungstenite::connect_async(
            &format!("ws://127.0.0.1:8080/output_{port}")
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

    let mut cpp_input_buffer: IpcUnmanagedObject<triple_buffer::CppTripleBuffer> =
        IpcUnmanagedObject::new(name);
    let mut input_buffer = TripleBuffer::new(cpp_input_buffer.get_mut());

    let video_track = std::sync::Arc::new(TrackLocalStaticSample::new(
        RTCRtpCodecCapability {
            mime_type: MIME_TYPE_H264.to_owned(),
            ..Default::default()
        },
        "video".to_owned(),
        "webrtc-rs".to_owned(),
    ));

    //let Some(codec) = ffmpeg::Codec::find_encoder(ffmpeg::AVCodecID_AV_CODEC_ID_H264) else {
    let Some(codec) = ffmpeg::Codec::find_encoder_by_name("h264_videotoolbox") else {
        panic!("Can't find encoder codec");
    };
    println!("Using codec {}", codec.long_name());
    let mut scaler = ffmpeg::Scaler::new(
        1920,
        1080,
        ffmpeg::AV_PIX_FMT_BGRA,
        1920,
        1080,
        ffmpeg::AV_PIX_FMT_YUV420P,
    );
    let Ok(mut encoder) = ffmpeg::Encoder::new(&codec, ffmpeg::AV_PIX_FMT_YUV420P) else {
        panic!("Can't create encoder");
    };

    let video_track2 = video_track.clone();
    tokio::spawn(async move {
        while let Ok(msg) = from_gui.recv() {
            match msg {
                GuiToEncoderMsg::AddPeerConnection(peer_connection) => {
                    // Add this newly created track to the PeerConnection
                    let Ok(rtp_sender) = peer_connection
                        .add_track(std::sync::Arc::clone(&video_track2) as std::sync::Arc<dyn TrackLocal + Send + Sync>)
                        .await else {
                            panic!("Could not add video track to peer connection");
                        };

                    // Read incoming RTCP packets
                    // Before these packets are returned they are processed by interceptors. For things
                    // like NACK this needs to be called.
                    tokio::spawn(async move {
                        let mut rtcp_buf = vec![0u8; 1500];
                        while let Ok((_, _)) = rtp_sender.read(&mut rtcp_buf).await {}
                    });
                }
            }
        }
    });

    let mut ticker = tokio::time::interval(Duration::from_millis(1000 / 25));
    let mut pts = 1;
    loop {
        input_buffer.about_to_read();

        let bgra_frame = ffmpeg::Frame::new();
        bgra_frame
            .fill(
                &input_buffer.read().video_frame,
                ffmpeg::AV_PIX_FMT_BGRA,
                1920,
                1080,
                1,
                pts,
                ffmpeg::AVRational { num: 1, den: 25 },
            )
            .unwrap();
        let mut yuv420_frame = scaler.scale(&bgra_frame).unwrap();
        *yuv420_frame.pts() = pts;
        *yuv420_frame.time_base() = ffmpeg::AVRational { num: 1, den: 25 };

        encoder.send(&yuv420_frame).unwrap();

        while let Some(packet) = encoder.receive().unwrap() {
            video_track
                .write_sample(&webrtc::media::Sample {
                    data: bytes::Bytes::copy_from_slice(packet.data()),
                    duration: Duration::from_millis(1000 / 25),
                    ..Default::default()
                })
                .await
                .unwrap();
        }

        let _ = ticker.tick().await;
        pts += 1;
    }
}
