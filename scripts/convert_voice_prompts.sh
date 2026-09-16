#!/usr/bin/env bash
# 把 res/*.mp3 转成设备需要的本地播报片段：OGG Opus / 16 kHz / 单声道 / 60 ms 帧
#
# 用法（必须先 cd 到仓库根目录，或直接给出脚本绝对路径；在 WSL 里跑）：
#   bash scripts/convert_voice_prompts.sh
#
# ! 60 ms 帧是硬要求，不能省：AudioService::PlaySound() 用 OggDemuxer 解析本地片段，
# ! 回调里 frame_duration 固定按 60 写死，非 60 ms 的片段播不出来。
#
# 码率用 32k：源 mp3 就是 16 kHz / 32 kbps，输出再降到 16k 等于把源砍一半、白丢一代质量。
# 41 条片段按 32k 总共约 130 KB 左右，app 分区还有 1 MB 以上余量，不值得为这点空间牺牲音质。
#
# 产出全部落在 main/assets/common/，由 main/CMakeLists.txt 的
#   EMBED_FILES ${LANG_SOUNDS} ${COMMON_SOUNDS}
# 编进 app 分区；scripts/gen_lang.py 会按 <base>.ogg → Lang::Sounds::OGG_<BASE> 生成常量。
# 因此文件名只能是 ASCII 小写字母/数字/下划线，不能用连字符。

set -euo pipefail

cd "$(dirname "$0")/.."

src_dir=res
out_dir=main/assets/common
# 百：光照播报走档位、其余数值都在 0-99，用不上，故意跳过
skip_base="d100"

if [ ! -d "$src_dir" ]; then
    echo "找不到 $src_dir/ —— 请把 mp3 放在仓库根目录的 res/ 下" >&2
    exit 1
fi

mkdir -p "$out_dir"

count=0
for f in "$src_dir"/*.mp3; do
    base=$(basename "$f" .mp3)
    if [ "$base" = "$skip_base" ]; then
        echo "跳过 $base（本项目用不到）"
        continue
    fi

    in_dur=$(ffprobe -v error -show_entries format=duration -of csv=p=0 "$f")
    ffmpeg -hide_banner -loglevel error -y -i "$f" \
        -af "silenceremove=start_periods=1:start_threshold=-50dB:start_silence=0.05,areverse,silenceremove=start_periods=1:start_threshold=-50dB:start_silence=0.05,areverse" \
        -c:a libopus -b:a 32k -ac 1 -ar 16000 -frame_duration 60 \
        "$out_dir/$base.ogg"
    out_dur=$(ffprobe -v error -show_entries format=duration -of csv=p=0 "$out_dir/$base.ogg")
    printf "%-20s in=%5.2fs out=%5.2fs\n" "$base" "$in_dur" "$out_dur"
    count=$((count + 1))
done

# 「播放提示音」命令词要的提示音：直接合成，不需要录音
ffmpeg -hide_banner -loglevel error -y -f lavfi -i "sine=frequency=880:duration=0.4" \
    -af "afade=t=out:st=0.15:d=0.25,volume=0.35" \
    -c:a libopus -b:a 16k -ac 1 -ar 16000 -frame_duration 60 "$out_dir/tone_chime.ogg"

echo "=== 已转换 $count 条 + tone_chime.ogg ==="
echo "$out_dir 下 ogg 总数：$(ls "$out_dir"/*.ogg | wc -l)"

echo "=== 编码参数（Opus 流 ffprobe 一律报 48000，这是编解码器内部采样率，不是问题）==="
for f in "$out_dir"/popup.ogg "$out_dir"/ev_crash.ogg "$out_dir"/d6.ogg "$out_dir"/tone_chime.ogg; do
    [ -f "$f" ] || continue
    dur=$(ffprobe -v error -show_entries format=duration -of csv=p=0 "$f")
    sr=$(ffprobe -v error -select_streams a:0 -show_entries stream=sample_rate -of csv=p=0 "$f")
    codec=$(ffprobe -v error -select_streams a:0 -show_entries stream=codec_name -of csv=p=0 "$f")
    nf=$(ffprobe -v error -select_streams a:0 -count_frames -show_entries stream=nb_read_frames -of csv=p=0 "$f")
    printf "%-18s codec=%-6s sr=%-6s dur=%-7s frames=%s\n" "$(basename "$f")" "$codec" "$sr" "$dur" "$nf"
done

echo "=== 帧长校验（frames × 0.06 ≈ duration 才算 60 ms 帧）==="
ok=0
bad=0
for f in "$out_dir"/*.ogg; do
    dur=$(ffprobe -v error -show_entries format=duration -of csv=p=0 "$f")
    nf=$(ffprobe -v error -select_streams a:0 -count_frames -show_entries stream=nb_read_frames -of csv=p=0 "$f")
    result=$(awk -v d="$dur" -v n="$nf" 'BEGIN{ want=d/0.06; diff=want-n; if (diff<0) diff=-diff; if (diff<1.5) print "ok"; else print "bad" }')
    if [ "$result" = "ok" ]; then
        ok=$((ok + 1))
    else
        bad=$((bad + 1))
        printf "可疑：%-24s frames=%-4s dur=%ss（期望帧数 ≈ %.1f）\n" "$(basename "$f")" "$nf" "$dur" \
            "$(awk -v d="$dur" 'BEGIN{print d/0.06}')"
    fi
done
echo "60 ms 帧符合：$ok 个；可疑：$bad 个"
