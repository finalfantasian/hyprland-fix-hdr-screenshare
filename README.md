# hyprland-fix-hdr-screenshare

Small Hyprland plugin that disables the HDR unmodified-copy MRT path used for
screenshare and screenshots.

It works around two HDR/FP16 issues:

- screenshare and screenshots freezing after some time
- blur under transparent windows not working correctly in HDR

Hyprland's built-in `render:keep_unmodified_copy = 0` disables this path
globally. This plugin does it by hooking `CMonitor::needsUnmodifiedCopy()`,
which avoids the broken HDR/FP16 mirror texture path while keeping non-HDR
monitors usable.

There is no dispatcher. Loading the plugin enables the workaround immediately.
Unloading the plugin removes the hook and restores Hyprland's normal behavior.
