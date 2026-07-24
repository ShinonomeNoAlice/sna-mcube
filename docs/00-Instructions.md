# Configs & Settings

## `musikcube` side - `Wasapi Exclusive IOutput`

| Config name                      | Meaning                                                                                                                           | Recommendation          | Rationale                                                                                                                                                                                                                                                                                                               |
| -------------------------------- | --------------------------------------------------------------------------------------------------------------------------------- | ----------------------- | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `enable_trace_logging`           | Hose down more detailed logging into `%APPDATA/musikcube`.                                                                        | `false`                 | Unless you bathe in disk space, or are triaging, keep this off.                                                                                                                                                                                                                                                         |
| `buffer_length_seconds`          | Capacity of WASAPI EM ring buffer in seconds.                                                                                     | `0.10`                  | "It works on my machine". Bump it higher if you're getting crackles.                                                                                                                                                                                                                                                    |
| `enable_audio_endpoint_routing`  | Consult `musikcube` for this one's functionality.                                                                                 | `false`                 | I don't need automatic changing behaviour. I like my determinism.                                                                                                                                                                                                                                                       |
| `headroom_db`                    | How much to duck your audio after it's finished processing.                                                                       | `-6.0`                  | Certain **doujin** test tracks of mine can peak above 0dBFS to +4dBTP after reconstruction with `soxr`, so -6dB is just headroom and safety margin. This is **NOT** soxr's fault. If your track reconstructs to +6 dBTP, the mastering engineer owes your DAC an apology. Unless it's doujin, then you buy them a beer. |
| `dac_settling_ms`                | Silent buffer in to be inserted when your DAC needs to re-lock its PLL on SR changes to ensure it doesn't skip playback.          | `800`                   | "It works on my FiiO K11 R2R". If your DAC actively locks playback until it's ready, set to `0`.                                                                                                                                                                                                                        |
| `release_device_on_pause`        | Whether to release the WASAPI EM lock on pausing.                                                                                 | `true`                  | Makes your life a little bit easier not having to kill `musikcube` to listen to something else.                                                                                                                                                                                                                         |
| `soxr_oversampling`              | Oversampling mode for `soxr`. Supports 0x to 16x, and pedal-to-the-metal maxouts (both integer scaling and literal maxxing out)   | `Max (Integer Scaling)` | Yes, my DAC has OS mode. But then I'd need to trust their DSP engineer to not mangle the OS implementation. So I use `soxr` + NOS, at least that way I know who to blame (me). Staying integer scaling avoids unnecessary cross-family (44.1kHz ↔ 48kHz) conversions. For the specifics, ask your DSP textbook.         |
| `soxr_preset`                    | `soxr`'s oversampling quality preset. Check their docs!                                                                           | `Very High`             | `soxr` is dirt cheap computationally. If my laptop Broadwell i5-5200U CPU can comfortably run VHQ I don't think yours would struggle. Unless you're on 2005-era hardware. How are you running this btw?                                                                                                                 |
| `soxr_custom_precision_bits`     | Applicable only if you set "Preset" to "Custom". Target bit precision with using `soxr`                                           | N/A                     | I just use the preset. I'm not that deep into DSP. Not yet anyway.                                                                                                                                                                                                                                                      |
| `soxr_custom_phase_response_pct` | Applicable only if you set "Preset" to "Custom". `soxr`'s filter's phase response.                                                | N/A                     | Honestly if you're reading this you're better off reading `soxr` (or SoX)'s docs. I'm not qualified to talk about this.                                                                                                                                                                                                 |
| `soxr_custom_passband_end`       | Applicable only if you set "Preset" to "Custom". `soxr`'s passband cutoff frequency as fraction of Nyquist frequency.             | N/A                     | Why are you still here? Go, shoo, go read their docs. Maybe grab your search engine on the way out. Why are you looking at me like that?                                                                                                                                                                                |
| `soxr_custom_stopband_begin`     | Applicable only if you set "Preset" to "Custom". `soxr`'s stopband start frequency relative to Nyquist.                           | N/A                     | Fine. Here's a joke. Why is casually watching movies from discs considered kinky?                                                                                                                                                                                                                                       |
| `soxr_custom_double_precision`   | Applicable only if you set "Preset" to "Custom". `Toggles 64-bit double-precision floating-point internal calculations for`soxr`. | N/A                     | Because you've got your _BD_ and your WASAPI _SM_.                                                                                                                                                                                                                                                                      |
| `vst_enabled`                    | Whether to enable VST3 support. VST3 processing happens after `soxr`.                                                             | `true`                  | By default it doesn't cost you anything. And it's handy if you wanna add your own 300 Freedom Eagles EQ. Or meters. Or something else.                                                                                                                                                                                  |
| `vst_block_size`                 | Sets the fixed frame block size (512, 1024, 2048, 4096, or Passthrough) delivered to VST3 plugins per processing pass.            | `1024`                  | "It works on my MiniMeters and its Audio Server". Strikes a nice balance between buffer health and latency.                                                                                                                                                                                                             |

## VST3 side

Yes. VST3. **Only**, VST**3**. I'll consider other plugin architectures later.

VST chain is strictly linear. If you want a DAG, open an issue.

### The TOML file

Demonstration entry from my actual TOML. Chain is top-down.

```toml
[[chain]]
autoload = true
bypass = true
path = 'C:\Program Files\Common Files\VST3\MSED.vst3'
preset = 'D:\cube\presets\Voxengo_MSED.vstpreset'
show_ui = false
window_title = '02 - Mid-Side'
```

| Key            | Meaning                                                                                           |
| -------------- | ------------------------------------------------------------------------------------------------- |
| `autoload`     | Whether to automatic load the settings saved in the `.vstpreset` file you specified.              |
| `bypass`       | For when you cannot be arsed into clicking their bypass button,                                   |
| `path`         | The absolute path to the VST3. Use single-quotes so you don't have to deal with double-backslash! |
| `preset`       | The absolutely path to the `.vstpreset` file to save your preferred default config to.            |
| `show_ui`      | For when you don't want to see a plugin's windows. MiniMeter's Audio Server is one example.       |
| `window_title` | Helpful for when you have multiple instances of a VST in the chain. Safe than sorry.              |

#### "You mentioned `.vstpreset` and saving/autoloading. Where is it?"

- Saving: Focus the VST3 window you wanna save and hit
  <kbd>Ctrl</kbd>+<kbd>S</kbd>.
- Autoloading: Set to `true`. It will load your `.vstpreset` in.

#### "Do I have to close `musikcube` to change the chain?"

Hot-loading of VSTs is supported. Just trigger a write on the TOML file (saving,
etc.)

#### "So I just closed my VST3's window...."

Pull up the TOML file and trigger a write. Unfortunately `musikcube` doesn't let
plugins register shortcuts so we have to do this the long way round.

#### "Where is my TOML?"

Ah. That's why there's...

### The TOML manager

`tools/vst_toml_manager` has you covered. If you have `uv`, just
`uv run vst-toml-manager`. Saves you the hassle of manually typing things on
TOML. Trust me, you don't want that hell.
