ALSA Plugin: FIFO
=================

>part of the voice-engine to make an open source smart speaker

This plugin is similar to the ALSA built-in file plugin, while the FIFO plugin doesn't require a slave.
The plugin can use a FIFO (named pipe) or a normal file as a capture stream.

The primary goal is using the plugin with [voice-engine/ec](https://github.com/voice-engine/ec)
to make the output of Acoustic Echo Cancellion (AEC) easy to acess.