mod vlc;
mod vlc_c;

fn main() {
    let vlc_instance = vlc::Vlc::new();

    let media = vlc::Media::from_path(
        &vlc_instance,
        "/Users/jonathantanner/Downloads/Big_Buck_Bunny_1080_10s_1MB.webm",
    );

    let mut player = vlc::MediaPlayer::from_media(&media);

    println!("Loaded");

    player.play();

    println!("Playing");

    while player.is_playing() {
        std::thread::sleep(core::time::Duration::from_secs(1));
        /*
        sleep (1);
        let milliseconds = libvlc_media_player_get_time(player);
        int64_t seconds = milliseconds / 1000;
        int64_t minutes = seconds / 60;
        milliseconds -= seconds * 1000;
        seconds -= minutes * 60;

        printf("Current time: %" PRId64 ":%" PRId64 ":%" PRId64 "\n",
        minutes, seconds, milliseconds);
        */
    }

    println!("Finished");

    player.stop();
}
