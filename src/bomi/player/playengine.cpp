#include "playengine.hpp"
#include "playengine_p.hpp"
#include "app.hpp"
#include "audio/audionormalizeroption.hpp"
#include "subtitle/subtitlemodel.hpp"
#include "os/os.hpp"

extern void *discnav_ctx;

PlayEngine::PlayEngine()
: d(new Data(this)) {
    _Debug("Create audio/video plugins");
    d->ac = new AudioController(this);
    d->vp = new VideoProcessor;
    d->sr = new SubtitleRenderer;
    d->vr = new VideoRenderer;
    d->vr->setOverlay(d->sr);
    d->vr->setRenderFrameFunction([this] (Fbo *frame, Fbo* osd, const QMargins &m)
        { d->renderVideoFrame(frame, osd, m); });

    d->params.m_mutex = &d->mutex;

    auto isAss = [=] () {
        auto track = d->params.sub_tracks().selection();
        return track && track->codec().toLower() == "ass"_a;
    };

    connect(&d->params, &MrlState::video_offset_changed, d->vr, &VideoRenderer::setOffset);
    connect(&d->params, &MrlState::video_aspect_ratio_changed, d->vr, &VideoRenderer::setAspectRatio);
    connect(&d->params, &MrlState::video_crop_ratio_changed, d->vr, &VideoRenderer::setCropRatio);
    auto updateLetterBox = [=] (bool override)
        { d->mpv.setAsync("ass-force-margins", d->vr->overlayOnLetterbox() && override); };
    connect(&d->params, &MrlState::sub_display_changed, d->vr, [=] (auto sd) {
        d->vr->setOverlayOnLetterbox(sd == SubtitleDisplay::OnLetterbox);
        updateLetterBox(d->params.sub_override_ass_position());
        d->mpv.update();
    });
    connect(&d->params, &MrlState::video_zoom_changed, this, &PlayEngine::zoomChanged);
    auto setAlignment = [=] () {
        const auto v = _EnumData(d->params.video_vertical_alignment());
        const auto h = _EnumData(d->params.video_horizontal_alignment());
        d->vr->setAlignment(v | h);
    };
    connect(&d->params, &MrlState::video_horizontal_alignment_changed, d->vr, setAlignment);
    connect(&d->params, &MrlState::video_vertical_alignment_changed, d->vr, setAlignment);
    connect(&d->params, &MrlState::video_effects_changed, d->vr, [=] (auto e)
        { d->vr->setFlipped(e & VideoEffect::FlipH, e & VideoEffect::FlipV); });
    connect(&d->params, &MrlState::video_tracks_changed, this, [=] (StreamList list) {
        if (_Change(d->hasVideo, !list.isEmpty()))
            emit hasVideoChanged();
        d->info.video.setTracks(list);
    });

    connect(&d->params, &MrlState::audio_volume_changed, this, &PlayEngine::volumeChanged);
    connect(&d->params, &MrlState::audio_muted_changed, this, &PlayEngine::mutedChanged);
    connect(&d->params, &MrlState::play_speed_changed, this, &PlayEngine::speedChanged);
    connect(&d->params, &MrlState::audio_tracks_changed, this,
            [=] (StreamList list) { d->info.audio.setTracks(list); });
//    connect(&d->params, &MrlState::audio_volume_normalizer_changed,
//            d->ac, &AudioController::setNormalizerActivated);
    connect(&d->params, &MrlState::audio_equalizer_changed,
            d->ac, &AudioController::setEqualizer);

    connect(&d->params, &MrlState::sub_sync_changed, d->sr, &SubtitleRenderer::setDelay);
    connect(&d->params, &MrlState::sub_hidden_changed, d->sr, &SubtitleRenderer::setHidden);

    auto updatePos = [=] (double p, bool o) {
        d->mpv.setAsync("sub-pos", o || !isAss() ? qRound(p * 100) : 100);
        updateLetterBox(o);
        d->mpv.update();
    };
    auto updateScale = [=] (double s, bool o) {
        d->mpv.setAsync("sub-scale", o || !isAss() ? qMax(0., 1. + s) : 1.);
        d->mpv.update();
    };

    connect(&d->params, &MrlState::sub_override_ass_position_changed, this,
            [=] (bool override) { updatePos(d->params.sub_position(), override); });
    connect(&d->params, &MrlState::sub_position_changed, d->sr, [=] (double pos)
        { d->sr->setPos(pos); updatePos(pos,d->params.sub_override_ass_position()); });
    connect(&d->params, &MrlState::sub_alignment_changed, d->sr, [=] (auto a) {
        const auto top = a == VerticalAlignment::Top;
        d->sr->setTopAligned(top);
    });
    connect(&d->params, &MrlState::sub_style_overriden_changed, this, [=] (bool o) {
        d->mpv.setAsync("ass-style-override", o ? "force"_b : "yes"_b);
        d->mpv.update();
    });
    connect(&d->params, &MrlState::sub_scale_changed, this, [=] (double s) {
        auto style = d->subStyle;
        style.font.size = qBound(0.0, style.font.size * (1.0 + s), 1.0);
        d->sr->setStyle(style);
        updateScale(s, d->params.sub_override_ass_scale());
    });

    auto set_subs = [=] ()
        { d->info.subtitle.setTracks(d->params.sub_tracks(), d->params.sub_tracks_inclusive()); };
    connect(&d->params, &MrlState::sub_tracks_changed, this, [=] () {
        set_subs();
        updatePos(d->params.sub_position(), d->params.sub_override_ass_position());
        updateScale(d->params.sub_scale(), d->params.sub_override_ass_scale());
    });
    connect(&d->params, &MrlState::sub_tracks_inclusive_changed, this, set_subs);

    connect(this, &PlayEngine::beginChanged, this, &PlayEngine::endChanged);
    connect(this, &PlayEngine::durationChanged, this, &PlayEngine::endChanged);
    connect(this, &PlayEngine::endChanged, this,
            [=] () { if (_Change(d->end_s, end()/1000)) emit end_sChanged(); });

    auto checkDeint = [=] () {
        auto act = Unavailable;
        if (d->vp->isInputInterlaced())
            act = d->vp->isOutputInterlaced() ? Deactivated : Activated;
        d->info.video.deint()->setState(act);
    };
    connect(d->vp, &VideoProcessor::inputInterlacedChanged, this, checkDeint);
    connect(d->vp, &VideoProcessor::outputInterlacedChanged, this, checkDeint);
    connect(d->vp, &VideoProcessor::deintMethodChanged, this,
            [=] (auto m) { d->info.video.deint()->setDriver(_EnumName(m)); });

    connect(d->vp, &VideoProcessor::skippingChanged, this, [=] (bool skipping) {
        if (skipping) {
            d->pauseAfterSkip = isPaused();
            d->mpv.setAsync("mute", true);
            d->mpv.setAsync("pause", false);
            d->mpv.setAsync("speed", 100.0);
        } else {
            d->mpv.setAsync("speed", d->params.play_speed());
            d->mpv.setAsync("pause", d->pauseAfterSkip);
            d->mpv.setAsync("mute", d->params.audio_muted());
        }
        d->updateVideoSubOptions();
        d->post(Searching, skipping);
    }, Qt::DirectConnection);
    connect(d->vp, &VideoProcessor::seekRequested, this, &PlayEngine::seek);
    connect(d->vp, &VideoProcessor::fpsManimulated, &d->info.video,
            &VideoObject::setFpsManimulation, Qt::QueuedConnection);
    connect(d->vp, &VideoProcessor::hwdecChanged, this, [=] (const QString &api)
    {
        auto &video = d->info.video;
        auto hwState = [&] () {
            if (!d->hwdec) {
                Q_ASSERT(api.isEmpty());
                return Deactivated;
            }
            if (!api.isEmpty()) {
                Q_ASSERT(api == OS::hwAcc()->name());
                return Activated;
            }
            const auto codec = CodecIdInfo::fromData(video.codec()->type());
            if (OS::hwAcc()->supports(codec) && !d->hwCodecs.contains(codec))
                return Deactivated;
            return Unavailable;
        };
        auto hwacc = video.hwacc();
        hwacc->setState(hwState());
        hwacc->setDriver(api);
    }, Qt::QueuedConnection);

    connect(d->ac, &AudioController::inputFormatChanged, this,
            [=] () { d->info.audio.filter()->setFormat(d->ac->inputFormat()); });
    connect(d->ac, &AudioController::outputFormatChanged, this,
            [=] () { d->info.audio.output()->setFormat(d->ac->outputFormat()); });
    connect(d->ac, &AudioController::samplerateChanged, this,
            [=] (int sr) {
        d->info.audio.decoder()->setBitrate(d->mpv.get<int>("packet-audio-bitrate"));
        d->info.audio.output()->setSampleRate(sr, true);
    });
    connect(d->ac, &AudioController::gainChanged,
            &d->info.audio, &AudioObject::setNormalizer);

    connect(d->sr, &SubtitleRenderer::selectionChanged,
            this, &PlayEngine::subtitleSelectionChanged);

    d->updateMediaName();
    d->frames.measure.setTimer([=]()
        { d->info.video.output()->setFps(d->frames.measure.get()); }, 100000);
    connect(&d->info.frameTimer, &QTimer::timeout, this, [=] () {
        d->info.video.decoder()->setBitrate(d->mpv.get<int>("packet-video-bitrate"));
        d->info.video.setDelayedFrames(d->info.delayed);
        d->info.video.setDroppedFrames(d->mpv.get<int64_t>("vo-drop-frame-count"));
    });
    d->info.frameTimer.setInterval(100);

    _Debug("Make registrations and connections");

    discnav_ctx = this;
    d->mpv.create();
    d->observe();
    d->request();

    const auto hwdec = OS::hwAcc()->name().toLatin1();
    d->mpv.setOption("hwdec", hwdec.isEmpty() ? "no" : hwdec.data());
    d->mpv.setOption("fs", "no");
    d->mpv.setOption("input-cursor", "yes");
    d->mpv.setOption("softvol", "yes");
    d->mpv.setOption("softvol-max", "1000.0");
    d->mpv.setOption("sub-auto", "no");
    d->mpv.setOption("osd-level", "0");
    d->mpv.setOption("quiet", "yes");
    d->mpv.setOption("input-terminal", "no");
    d->mpv.setOption("ad-lavc-downmix", "no");
    d->mpv.setOption("title", "\"\"");
    d->mpv.setOption("audio-pitch-correction", "no");
    d->mpv.setOption("vo", d->vo(&d->params));
    d->mpv.setOption("af", d->af(&d->params));
    d->mpv.setOption("vf", d->vf(&d->params));
    d->mpv.setOption("hr-seek", d->preciseSeeking ? "yes" : "absolute");
    d->mpv.setOption("audio-file-auto", "no");
    d->mpv.setOption("sub-auto", "no");
    d->mpv.setOption("sub-text-margin-y", "0");
    d->mpv.setOption("audio-client-name", cApp.name());

    auto overrides = qgetenv("BOMI_MPV_OPTIONS").trimmed();
    if (!overrides.isEmpty()) {
        const auto opts = QString::fromLocal8Bit(overrides);
        const auto args = opts.split(QRegEx(uR"([\s\t]+)"_q),
                                     QString::SkipEmptyParts);
        for (int i=0; i<args.size(); ++i) {
            if (!args[i].startsWith("--"_a)) {
                _Error("Cannot parse option %%.", args[i]);
                continue;
            }
            const auto arg = args[i].midRef(2);
            const int index = arg.indexOf('='_q);
            if (index < 0) {
                if (arg.startsWith("no-"_a))
                    d->mpv.setOption(arg.mid(3).toLatin1(), "no");
                else
                    d->mpv.setOption(arg.toLatin1(), "yes");
            } else {
                const auto key = arg.left(index).toLatin1();
                const auto value = arg.mid(index+1).toLatin1();
                d->mpv.setOption(key, value);
            }
        }
    }
    d->mpv.initialize();
    _Debug("Initialized");
    d->hook();
    d->mpv.setUpdateCallback([=] ()
        { d->vr->updateForNewFrame(d->info.video.output()->size()); });
}

PlayEngine::~PlayEngine()
{
    qDeleteAll(d->info.chapters);
    qDeleteAll(d->info.editions);
    d->params.m_mutex = nullptr;
    d->mpv.destroy();
    d->vr->setOverlay(nullptr);
    delete d->ac;
    delete d->sr;
    delete d->vr;
    delete d->vp;
    delete d;
    _Debug("Finalized");
}

auto PlayEngine::initializeGL(QOpenGLContext *ctx) -> void
{
    d->mpv.initializeGL(ctx);
}

auto PlayEngine::finalizeGL(QOpenGLContext */*ctx*/) -> void
{
    d->mpv.finalizeGL();
}

auto PlayEngine::metaData() const -> const MetaData&
{
    return d->metaData;
}

auto PlayEngine::setSubtitleDelay(int ms) -> void
{
    if (d->params.set_sub_sync(ms))
        d->mpv.setAsync("sub-delay", ms * 1e-3);
}

auto PlayEngine::cacheSize() const -> int
{
    return d->cache.size;
}

auto PlayEngine::cacheUsed() const -> int
{
    return d->cache.used;
}

auto PlayEngine::begin() const -> int
{
    return d->begin;
}

auto PlayEngine::end() const -> int
{
    return d->begin + d->duration;
}

auto PlayEngine::duration() const -> int
{
    return d->duration;
}

auto PlayEngine::edition() const -> EditionChapterObject*
{
    return &d->info.edition;
}

auto PlayEngine::editions() const -> const QVector<EditionObject*>&
{
    return d->info.editions;
}

auto PlayEngine::chapters() const -> const QVector<ChapterObject*>&
{
    return d->info.chapters;
}

auto PlayEngine::setPriority_locked(const QStringList &audio, const QStringList &sub) -> void
{
    d->streams[StreamAudio].priority = audio;
    d->streams[StreamSubtitle].priority = sub;
}

auto PlayEngine::setAutoloader_locked(const Autoloader &audio, const Autoloader &sub) -> void
{
    d->streams[StreamAudio].autoloader = audio;
    d->streams[StreamSubtitle].autoloader = sub;
}

auto PlayEngine::clearAllSubtitleSelection() -> void
{
    const int id = d->params.sub_tracks().selectionId();
    if (id >= 0)
        setSubtitleTrackSelected(id, false);
    d->sr->deselect(-1);
    d->syncInclusiveSubtitles();
}

auto PlayEngine::setSubtitleInclusiveTrackSelected(int id, bool s) -> void
{
    if (s)
        d->sr->select(id);
    else
        d->sr->deselect(id);
    d->syncInclusiveSubtitles();
    emit d->params.currentTrackChanged(StreamInclusiveSubtitle);
}

auto PlayEngine::setSubtitleTrackSelected(int id, bool s) -> void
{
    if (s)
        d->mpv.setAsync("sid", id);
    else if (d->params.sub_tracks().selectionId() == id)
        d->mpv.setAsync("sid", "no"_b);
}

auto PlayEngine::setTrackSelected(StreamType type, int id, bool s) -> void
{
    switch (type) {
    case StreamVideo:
        setVideoTrackSelected(id, s);
        break;
    case StreamAudio:
        setAudioTrackSelected(id, s);
        break;
    case StreamSubtitle:
        setSubtitleTrackSelected(id, s);
        break;
    case StreamInclusiveSubtitle:
        setSubtitleInclusiveTrackSelected(id, s);
        break;
    default:
        return;
    }
//    emit currentTrackChanged(type, id);
}

auto PlayEngine::autoloadSubtitleFiles() -> void
{
    clearSubtitleFiles();
    MpvFileList files; QVector<SubComp> loads;
    d->mutex.lock();
    _R(files, loads) = d->autoloadSubtitle(&d->params);
    d->mutex.unlock();
    for (auto &file : files.names) {
        d->mpv.setAsync("options/subcp", d->assEncodings[file].name().toLatin1());
        d->mpv.tellAsync("sub_add", MpvFile(file), "auto"_b);
    }
    d->setInclusiveSubtitles(loads);
}

auto PlayEngine::autoloadAudioFiles() -> void
{
    setAudioFiles(d->autoloadFiles(StreamAudio).names);
}

auto PlayEngine::reloadSubtitleFiles(const EncodingInfo &enc, bool detect) -> void
{
    d->mutex.lock();
    auto old1 = d->params.sub_tracks();
    auto old2 = d->params.sub_tracks_inclusive();
    d->mutex.unlock();
    clearSubtitleFiles();
    for (auto &track : old1) {
        if (track.isExternal())
            d->sub_add(track.file(), d->encoding(track, enc, detect), track.isSelected());
    }
    d->setInclusiveSubtitles(d->restoreInclusiveSubtitles(old2, enc, detect));
}

auto PlayEngine::reloadAudioFiles() -> void
{
    d->mutex.lock();
    auto old = d->params.audio_tracks();
    d->mutex.unlock();
    clearAudioFiles();
    for (auto &track : old) {
        if (track.isExternal())
            d->audio_add(track.file(), track.isSelected());
    }
}

auto PlayEngine::setSubtitleHidden(bool hidden) -> void
{
    if (d->params.set_sub_hidden(hidden))
        d->mpv.setAsync("sub-visibility", !hidden);
}

auto PlayEngine::setVideoTrackSelected(int id, bool s) -> void
{
    if (s)
        d->mpv.setAsync("vid", id);
    else if (d->params.video_tracks().selectionId() == id)
        d->mpv.setAsync("vid", -1);
}

auto PlayEngine::setAudioTrackSelected(int id, bool s) -> void
{
    if (s)
        d->mpv.setAsync("aid", id);
    else if (d->params.audio_tracks().selectionId() == id)
        d->mpv.setAsync("aid", -1);
}

auto PlayEngine::run() -> void
{
    d->mpv.start();
}

auto PlayEngine::waitUntilTerminated() -> void
{
    if (d->mpv.isRunning())
        d->mpv.wait();
}

auto PlayEngine::speed() const -> double
{
    return d->params.play_speed();
}

auto PlayEngine::setSpeed(double s) -> void
{
    if (d->params.set_play_speed(s)) {
        d->mpv.setAsync("speed", speed());
        d->resync();
    }
}

auto PlayEngine::setSubtitleScale(double by) -> void
{
    d->params.set_sub_scale(by);
}

auto PlayEngine::setSubtitleStyle_locked(const OsdStyle &style) -> void
{
    d->subStyle = style;
    d->updateSubtitleStyle();
}

auto PlayEngine::seek(int pos) -> void
{
    if (pos >= 0 && !d->hasImage)
        d->mpv.tell("seek", (double)pos/1000.0, 2);
    d->vp->stopSkipping();
}

auto PlayEngine::relativeSeek(int pos) -> void
{
    if (!d->hasImage) {
        d->mpv.tell("seek", (double)pos/1000.0, 0);
        emit sought();
    }
    d->vp->stopSkipping();
}

auto PlayEngine::setClippingMethod_locked(ClippingMethod method) -> void
{
    d->ac->setClippingMethod(method);
}

auto PlayEngine::setChannelLayoutMap_locked(const ChannelLayoutMap &map) -> void
{
    d->ac->setChannelLayoutMap(map);
}

auto PlayEngine::reload() -> void
{
    if (isStopped())
        return;
    d->mutex.lock();
    d->reload = d->time;
    d->mutex.unlock();
    load(d->mrl);
}

auto PlayEngine::setHistory(HistoryModel *history) -> void
{
    d->history = history;
}

auto PlayEngine::lock() -> void
{
    d->mutex.lock();
}

auto PlayEngine::unlock() -> void
{
    d->mutex.unlock();
}

auto PlayEngine::setChannelLayout(ChannelLayout layout) -> void
{
    if (d->params.set_audio_channel_layout(layout)) {
        d->mpv.setAsync("options/audio-channels", ChannelLayoutInfo::data(layout));
        if (d->time > 0)
            d->mpv.tellAsync("ao_reload");
    }
}

auto PlayEngine::setAudioDevice_locked(const QString &device) -> void
{
    d->params.d->audioDevice = device;
    d->mpv.setAsync("options/audio-device", d->params.d->audioDevice.toLatin1());
}

auto PlayEngine::screen() const -> QQuickItem*
{
    return d->vr;
}

auto PlayEngine::setCache_locked(const CacheInfo &info) -> void
{
    d->params.d->cache = info;
}

auto PlayEngine::setSmbAuth_locked(const SmbAuth &smb) -> void
{
    d->params.d->smb = smb;
}

auto PlayEngine::setHwAcc_locked(bool use, const QList<CodecId> &codecs) -> void
{
    d->hwdec = use;
    d->hwCodecs = codecs;

    QByteArray hwcdc;
    for (auto c : codecs)
        hwcdc += _EnumData(c).toLatin1() + ',';
    hwcdc.chop(1);
    d->mpv.setAsync("options/hwdec-codecs", use ? hwcdc : ""_b);
}

auto PlayEngine::avSync() const -> int
{
    return d->avSync;
}

auto PlayEngine::stepFrame(int direction) -> void
{
    if ((d->state & (Playing | Paused)) && d->seekable)
        d->mpv.tellAsync(direction > 0 ? "frame_step"_b : "frame_back_step"_b);
}

auto PlayEngine::isWaiting() const -> bool
{
    return d->waitings;
}

auto PlayEngine::waiting() const -> Waiting
{
    if (d->waitings & Searching)
        return Searching;
    if (d->waitings & Buffering)
        return Buffering;
    if (d->waitings & Loading)
        return Loading;
    return NoWaiting;
}

auto PlayEngine::state() const -> State
{
    return d->state;
}

auto PlayEngine::customEvent(QEvent *event) -> void
{
    d->process(event);
}

auto PlayEngine::media() const -> MediaObject*
{
    return &d->info.media;
}

auto PlayEngine::audio() const -> AudioObject*
{
    return &d->info.audio;
}

auto PlayEngine::video() const -> VideoObject*
{
    return &d->info.video;
}

auto PlayEngine::subtitle() const -> SubtitleObject*
{
    return &d->info.subtitle;
}

auto PlayEngine::seekChapter(int number) -> void
{
    d->mpv.setAsync("chapter", number);
}

auto PlayEngine::seekEdition(int number, int from) -> void
{
    const auto mrl = d->mrl;
    if (number == DVDMenu && mrl.isDisc())
        d->mpv.tellAsync("discnav", "menu"_b);
    else if (0 <= number && number < d->info.editions.size()) {
        d->mpv.setAsync(mrl.isDisc() ? "disc-title"_b : "edition"_b, number);
        seek(from);
    }
}

auto PlayEngine::setAudioVolume(double volume) -> void
{
    if (d->params.set_audio_volume(volume))
        d->mpv.setAsync("volume", d->volume(&d->params));
}

auto PlayEngine::isMuted() const -> bool
{
    return d->params.audio_muted();
}

auto PlayEngine::volume() const -> double
{
    return d->params.audio_volume();
}

auto PlayEngine::setAudioAmp(double amp) -> void
{
    if (d->params.set_audio_amplifier(amp))
        d->mpv.setAsync("volume", d->volume(&d->params));
}

auto PlayEngine::setAudioMuted(bool muted) -> void
{
    if (d->params.set_audio_muted(muted))
        d->mpv.setAsync("mute", muted);
}

auto PlayEngine::shutdown() -> void
{
    d->mpv.tell("quit", 1);
}

auto PlayEngine::setResume_locked(bool resume) -> void
{
    d->resume = resume;
}

auto PlayEngine::setPreciseSeeking_locked(bool on) -> void
{
    if (_Change(d->preciseSeeking, on))
        d->mpv.setAsync("options/hr-seek", on ? "yes"_b : "absolute"_b);
}

auto PlayEngine::setMrl(const Mrl &mrl) -> void
{
    if (d->mrl != mrl) {
        stop();
        d->mrl = mrl;
        d->updateMediaName();
        emit mrlChanged(d->mrl);
    }
}

auto PlayEngine::load(const Mrl &mrl, bool tryResume) -> void
{
    if (_Change(d->mrl, mrl)) {
        d->hasImage = mrl.isImage();
        d->updateMediaName();
        emit mrlChanged(d->mrl);
    }
    if (!d->mrl.isEmpty())
        d->loadfile(d->mrl, tryResume);
}

auto PlayEngine::time() const -> int
{
    return d->time;
}

auto PlayEngine::isSeekable() const -> bool
{
    return d->seekable;
}

auto PlayEngine::hasVideoFrame() const -> bool
{
    return d->vr->hasFrame();
}

auto PlayEngine::hasVideo() const -> bool
{
    return d->hasVideo;
}

auto PlayEngine::chapter() const -> EditionChapterObject*
{
    return &d->info.chapter;
}

auto PlayEngine::pause() -> void
{
    if (d->hasImage)
        d->post(Paused);
    else
        d->mpv.set("pause", true);
    d->pauseAfterSkip = true;
    d->vp->stopSkipping();
}

auto PlayEngine::unpause() -> void
{
    if (d->hasImage)
        d->post(Playing);
    else
        d->mpv.set("pause", false);
}

auto PlayEngine::mrl() const -> Mrl
{
    return d->mrl;
}

auto PlayEngine::setAudioSync(int sync) -> void
{
    if (d->params.set_audio_sync(sync))
        d->mpv.setAsync("audio-delay", sync * 1e-3);
}

auto PlayEngine::setResyncAvWhenFilterToggled_locked(bool on) -> void
{
    d->filterResync = on;
}

auto PlayEngine::setAudioVolumeNormalizer(bool on) -> void
{
    if (d->params.set_audio_volume_normalizer(on)) {
        d->mpv.tellAsync("af", "set"_b, d->af(&d->params));
        d->resync(true);
    }
}

auto PlayEngine::setAudioTempoScaler(bool on) -> void
{
    if (d->params.set_audio_tempo_scaler(on)) {
        d->mpv.tellAsync("af", "set"_b, d->af(&d->params));
        d->resync();
    }
}

auto PlayEngine::stop() -> void
{
    d->mpv.tell("stop");
}

auto PlayEngine::setMotionIntrplOption_locked(const MotionIntrplOption &option)
-> void
{
    d->vp->setMotionIntrplOption(option);
}

auto PlayEngine::setVolumeNormalizerOption_locked(const AudioNormalizerOption &option)
-> void
{
    d->ac->setNormalizerOption(option);
}

auto PlayEngine::setDeintOptions_locked(const DeintOptionSet &set) -> void
{
    d->params.d->deint = set;
    emit deintOptionsChanged();
}

auto PlayEngine::videoSizeHint() const -> QSize
{
    return d->vr->sizeHint();
}

auto PlayEngine::setVideoOffset(const QPointF &offset) -> void
{
    d->params.set_video_offset(offset);
}

auto PlayEngine::videoOutputAspectRatio() const -> double
{
    return d->vr->outputAspectRatio();
}

auto PlayEngine::setVideoAspectRatio(double ratio) -> void
{
    d->params.set_video_aspect_ratio(ratio);
}

auto PlayEngine::setVideoCropRatio(double ratio) -> void
{
    d->params.set_video_crop_ratio(ratio);
}

auto PlayEngine::setVideoVerticalAlignment(VerticalAlignment a) -> void
{
    d->params.set_video_vertical_alignment(a);
}

auto PlayEngine::setVideoHorizontalAlignment(HorizontalAlignment a) -> void
{
    d->params.set_video_horizontal_alignment(a);
}

auto PlayEngine::setVideoZoom(double zoom) -> void
{
    d->params.set_video_zoom(zoom);
}

auto PlayEngine::videoZoom() const -> double
{
    return d->params.video_zoom();
}

auto PlayEngine::setDeintMode(DeintMode mode) -> void
{
    if (d->params.set_video_deinterlacing(mode)) {
        if (isPaused()) {
            d->mpv.setAsync("deinterlace", !!(int)mode);
            d->refresh();
        } else
            d->mpv.setAsync("deinterlace", !!(int)mode);
    }
}

auto PlayEngine::deintMode() const -> DeintMode
{
    return d->params.video_deinterlacing();
}

auto PlayEngine::sendMouseClick(const QPointF &pos) -> void
{
    d->setMousePos(pos);
    if (d->params.d->disc)
        d->mpv.tellAsync("discnav", "mouse"_b);
}

auto PlayEngine::sendMouseMove(const QPointF &pos) -> void
{
    if (d->setMousePos(pos))
        d->mpv.tellAsync("discnav", "mouse_move"_b);
}

auto PlayEngine::audioDeviceList() const -> QList<AudioDevice>
{
    const QVariantList list = d->mpv.get<QVariant>("audio-device-list").toList();
    QList<AudioDevice> devs;
    devs.reserve(list.size());
    for (auto &one : list) {
        const auto map = one.toMap();
        AudioDevice dev;
        dev.name = map[u"name"_q].toString();
        dev.description = map[u"description"_q].toString();
        devs.push_back(dev);
    }
    return devs;
}

auto PlayEngine::setYle(YleDL *yle) -> void
{
    d->yle = yle;
}

auto PlayEngine::setYouTube(YouTubeDL *yt) -> void
{
    d->youtube = yt;
}

auto PlayEngine::setColorRange(ColorRange range) -> void
{
    if (d->params.set_video_range(range))
        d->mpv.setAsync("colormatrix-input-range", _EnumData(range).option);
}

auto PlayEngine::setColorSpace(ColorSpace space) -> void
{
    if (d->params.set_video_space(space))
        d->mpv.setAsync("colormatrix", _EnumData(space).option);
}

auto PlayEngine::setMotionInterpolation(bool on) -> void
{
    if (d->params.set_video_motion_interpolation(on)) {
        d->mpv.tellAsync("vf", "set"_b, d->vf(&d->params));
        d->updateVideoSubOptions();
    }
}

auto PlayEngine::setInterpolator(const IntrplParamSet &params) -> void
{
    d->mutex.lock();
    auto changed = _Change(d->params.d->intrpl[params.type], params);
    d->mutex.unlock();
    if (changed | d->params.set_video_interpolator(params.type))
        d->updateVideoSubOptions();
}

auto PlayEngine::setChromaUpscaler(const IntrplParamSet &params) -> void
{
    d->mutex.lock();
    auto changed = _Change(d->params.d->chroma[params.type], params);
    d->mutex.unlock();
    if (changed | d->params.set_video_chroma_upscaler(params.type))
        d->updateVideoSubOptions();
}

auto PlayEngine::interpolator() const -> IntrplParamSet
{
    return d->params.d->intrpl[d->params.video_interpolator()];
}

auto PlayEngine::chromaUpscaler() const -> IntrplParamSet
{
    return d->params.d->chroma[d->params.video_chroma_upscaler()];
}

auto PlayEngine::interpolatorMap() const -> IntrplParamSetMap
{
    return d->params.d->intrpl;
}

auto PlayEngine::chromaUpscalerMap() const -> IntrplParamSetMap
{
    return d->params.d->chroma;
}

auto PlayEngine::setInterpolatorMap(const IntrplParamSetMap &map) -> void
{
    d->params.d->intrpl = map;
}

auto PlayEngine::setChromaUpscalerMap(const IntrplParamSetMap &map) -> void
{
    d->params.d->chroma = map;
}

auto PlayEngine::setInterpolator(Interpolator type) -> void
{
    if (d->params.set_video_interpolator(type))
        d->updateVideoSubOptions();
}

auto PlayEngine::setChromaUpscaler(Interpolator type) -> void
{
    if (d->params.set_video_chroma_upscaler(type))
        d->updateVideoSubOptions();
}

auto PlayEngine::setVideoDithering(Dithering dithering) -> void
{
    if (d->params.set_video_dithering(dithering))
        d->updateVideoSubOptions();
}

auto PlayEngine::setVideoEqualizer(const VideoColor &eq) -> void
{
    if (d->params.set_video_color(eq))
        d->updateVideoSubOptions();
}

auto PlayEngine::setVideoEffects(VideoEffects e) -> void
{
    if (d->params.set_video_effects(e))
        d->updateVideoSubOptions();
}

auto PlayEngine::takeSnapshot() -> void
{
    d->ss.take = true;
    d->vr->updateForNewFrame(d->displaySize());
}

auto PlayEngine::snapshot(QImage *frame, QImage *osd) -> void
{
    *frame = d->ss.frame;
    *osd = d->ss.osd;
}

auto PlayEngine::clearSnapshots() -> void
{
    d->ss.frame = d->ss.osd = QImage();
}

auto PlayEngine::setVideoHighQualityDownscaling(bool on) -> void
{
    if (d->params.set_video_hq_downscaling(on))
        d->updateVideoSubOptions();
}

auto PlayEngine::setVideoHighQualityUpscaling(bool on) -> void
{
    if (d->params.set_video_hq_upscaling(on))
        d->updateVideoSubOptions();
}

auto PlayEngine::seekToNextBlackFrame() -> void
{
    if (!isStopped())
        d->vp->skipToNextBlackFrame();
}

auto PlayEngine::waitingText() const -> QString
{
    switch (waiting()) {
    case Loading:
        return tr("Loading");
    case Searching:
        return tr("Searching");
    case Buffering:
        return tr("Buffering");
    case Seeking:
        return tr("Seeking");
    default:
        return QString();
    }
}

auto PlayEngine::stateText() const -> QString
{
    switch (d->state) {
    case Stopped:
        return tr("Stopped");
    case Playing:
        return tr("Playing");
    case Paused:
        return tr("Paused");
    case Error:
        return tr("Error");
    default:
        return QString();
    }
}

auto PlayEngine::setAudioEqualizer(const AudioEqualizer &eq) -> void
{
    d->params.set_audio_equalizer(eq);
}

template<class L, class T = EditionChapterObject>
SIA makePtrList(const QObject *o, const L *list) -> QQmlListProperty<T>
{
    auto at = [] (QQmlListProperty<T> *p, int index) -> T*
        { return static_cast<const L*>(p->data)->value(index); };
    auto count = [] (QQmlListProperty<T> *p) -> int
        { return static_cast<const L*>(p->data)->size(); };
    return QQmlListProperty<T>(const_cast<QObject*>(o), const_cast<L*>(list), count, at);
}

auto PlayEngine::editionList() const -> QQmlListProperty<EditionChapterObject>
{
    return makePtrList(this, &d->info.editions);
}

auto PlayEngine::chapterList() const -> QQmlListProperty<EditionChapterObject>
{
    return makePtrList(this, &d->info.chapters);
}

auto PlayEngine::time_s() const -> int
{
    return d->time_s;
}

auto PlayEngine::duration_s() const -> int
{
    return d->duration_s;
}

auto PlayEngine::begin_s() const -> int
{
    return d->begin_s;
}

auto PlayEngine::end_s() const -> int
{
    return d->end_s;
}

auto PlayEngine::setAudioFiles(const QStringList &files) -> void
{
    clearAudioFiles();
    addAudioFiles(files);
}

auto PlayEngine::addAudioFiles(const QStringList &files) -> void
{
    for (auto file : files)
        d->mpv.tellAsync("audio_add", MpvFile(file), "auto"_b);
}

auto PlayEngine::clearAudioFiles() -> void
{
    for (auto &track : d->params.audio_tracks()) {
        if (track.isExternal())
            d->mpv.tellAsync("audio_remove", track.id());
    }
}

auto PlayEngine::params() const -> const MrlState*
{
    return &d->params;
}

auto PlayEngine::setAutoselectMode_locked(bool enable, AutoselectMode mode,
                                       const QString &ext) -> void
{
    d->params.d->autoselect = enable;
    d->params.d->autoselectMode = mode;
    d->params.d->autoselectExt = ext;
}

auto PlayEngine::setSubtitleAlignment(VerticalAlignment a) -> void
{
    d->params.set_sub_alignment(a);
    d->params.set_sub_position(d->sr->pos() * 100);
}

auto PlayEngine::setSubtitlePosition(double pos) -> void
{
    d->params.set_sub_position(pos);
}

auto PlayEngine::captionBeginTime() -> int
{
    return d->sr->start(d->time);
}

auto PlayEngine::captionEndTime() -> int
{
    return d->sr->finish(d->time);
}

auto PlayEngine::captionBeginTime(int direction) -> int
{
    if (direction < 0)
        return d->sr->previous();
    if (direction > 0)
        return d->sr->next();
    return d->sr->current();
}

auto PlayEngine::subtitleImage(const QRect &rect, QRectF *subRect) const -> QImage
{
    return d->sr->draw(rect, subRect);
}

auto PlayEngine::setSubtitleDisplay(SubtitleDisplay sd) -> void
{
    d->params.set_sub_display(sd);
}

auto PlayEngine::setOverrideAssTextStyle(bool overriden) -> void
{
    d->params.set_sub_style_overriden(overriden);
}

auto PlayEngine::setOverrideAssPosition(bool override) -> void
{
    d->params.set_sub_override_ass_position(override);
}

auto PlayEngine::setOverrideAssScale(bool override) -> void
{
    d->params.set_sub_override_ass_scale(override);
}

auto PlayEngine::setSubtitleFiles(const QStringList &files,
                                  const EncodingInfo &encoding) -> void
{
    clearSubtitleFiles();
    addSubtitleFiles(files, encoding);
}

auto PlayEngine::addSubtitleFiles(const QStringList &files,
                                  const EncodingInfo &encoding) -> void
{
    QVector<SubtitleWithEncoding> subs(files.size());
    for (int i = 0; i < files.size(); ++i) {
        subs[i].file = files[i];
        subs[i].encoding = encoding;
    }
    d->addSubtitleFiles(subs);
}

auto PlayEngine::clearSubtitleFiles() -> void
{
    for (auto &track : d->params.sub_tracks()) {
        if (track.isExternal())
            d->mpv.tellAsync("sub_remove", track.id());
    }
    d->setInclusiveSubtitles(QVector<SubComp>());
}

auto PlayEngine::restore(const MrlState *params) -> void
{
    QMutexLocker l(&d->mutex);
    d->params.m_mutex = nullptr;
    d->params.blockSignals(true);
    d->params.copyFrom(params);
    d->params.blockSignals(false);
    d->params.m_mutex = &d->mutex;
    d->params.notifyAll();
}

auto PlayEngine::subtitleSelection() const -> QVector<SubComp>
{
    return d->sr->selection();
}

auto PlayEngine::default_() const -> const MrlState*
{
    return &d->default_;
}

auto PlayEngine::isMouseInButton() const -> bool
{
    return d->mouseInButton && d->params.d->disc;
}
