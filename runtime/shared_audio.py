"""Session-local mixing for the stock RK817's otherwise exclusive playback PCM."""
from pathlib import Path


def configure(app, env, stock=Path('/etc/asound.conf')):
    # Leave custom audio setups and non-stock platforms alone. This override
    # extends the known asym profile, preserving its microphone definition.
    if env.get('ALSA_CONFIG_PATH') or env.get('SDL_AUDIODRIVER', 'alsa') != 'alsa':
        return env
    home = Path(env.get('HOME', str(Path.home())))
    user_config = Path(env.get('XDG_CONFIG_HOME', str(home / '.config'))) / 'alsa/asoundrc'
    if (home / '.asoundrc').exists() or user_config.exists():
        return env
    config = Path(app) / 'runtime/alsa-rgds.conf'
    try:
        profile = Path(stock).read_text()
        known = all(value in profile for value in
                    ('pcm.!default', 'type asym', 'playback.pcm', 'capture.pcm',
                     'slave.pcm "hw:0,0"', 'name "Playback Path"', 'value "SPK"'))
        if known and config.is_file():
            env['ALSA_CONFIG_PATH'] = str(config)
            env['RGDS_SHARED_AUDIO'] = '1'
    except OSError:
        pass
    return env
