# tools/qiaopi/adpcm.py —— IMA-ADPCM 参考实现（编码器 + 解码器）。
#
# 与 main/qpq_adpcm.c 的解码器逐位对应。两边的等价性不是靠约定，而是靠
# tests/qpq_adpcm_fixture.h 里那组固定向量：它由这里的 encode 产出、由这里的
# decode 记录期望值，C 解码器必须解出同样的序列（tests/test_qpq_adpcm.c 断言）。
# 改任何一侧都会立刻抓住另一侧。
#
# 片段格式（4 字节头 + 半字节流）：
#   [0:2]  首样本，int16 小端。首样本本身无损：解码器直接吐它，不消耗 nibble。
#   [2]    初始步长索引（0..88）。
#   [3]    预留，恒 0。
#   [4:]   其余每个样本一个 nibble，每字节先高后低（与 IMA 一致）。
import struct

# IMA 步长表，89 项。与 main/qpq_adpcm.c 的 s_step_table 逐项相同。
STEP_TABLE = [
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17,
    19, 21, 23, 25, 28, 31, 34, 37, 41, 45,
    50, 55, 60, 66, 73, 80, 88, 97, 107, 118,
    130, 143, 157, 173, 190, 209, 230, 253, 279, 307,
    337, 371, 408, 449, 494, 544, 598, 658, 724, 796,
    876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066,
    2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358,
    5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899,
    15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767,
]

# 步长索引调整表，按 nibble 低三位索引。与 C 的 s_index_table 相同。
INDEX_TABLE = [-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8]

HEADER_BYTES = 4
STEP_INDEX_MAX = len(STEP_TABLE) - 1
PREDICTOR_MIN = -32768
PREDICTOR_MAX = 32767


def clamp_step_index(value):
    if value < 0:
        return 0
    if value > STEP_INDEX_MAX:
        return STEP_INDEX_MAX
    return value


def clamp_predictor(value):
    if value < PREDICTOR_MIN:
        return PREDICTOR_MIN
    if value > PREDICTOR_MAX:
        return PREDICTOR_MAX
    return value


def encoded_size(sample_count):
    """N 个样本编码后占多少字节；与 C 的 qpq_adpcm_encoded_size 一致。"""
    if sample_count == 0:
        return 0
    return HEADER_BYTES + (sample_count // 2)


def decode_nibble(nibble, predictor, step_index):
    """解一个 nibble，返回 (新预测值, 新步长索引)。

    重建量用的是**更新前**的步长，与 C 解码器同一件事：编码器与解码器必须对
    同一个 nibble 得到同一个预测值，否则编出来的字节解回放就是错的。
    """
    step = STEP_TABLE[clamp_step_index(step_index)]
    reconstructed = step >> 3
    if nibble & 4:
        reconstructed += step
    if nibble & 2:
        reconstructed += step >> 1
    if nibble & 1:
        reconstructed += step >> 2
    if nibble & 8:
        predictor -= reconstructed
    else:
        predictor += reconstructed
    return (clamp_predictor(predictor),
            clamp_step_index(step_index + INDEX_TABLE[nibble & 7]))


def encode(samples, step_index=0):
    """把 int16 样本序列编码成一串字节。空序列编成空串。"""
    n = len(samples)
    if n == 0:
        return b""
    first = int(samples[0])
    if first < PREDICTOR_MIN or first > PREDICTOR_MAX:
        raise ValueError("首样本越出 int16：%d" % first)

    out = bytearray(struct.pack("<hB", first, clamp_step_index(step_index)))
    out.append(0)

    predictor = first
    index = clamp_step_index(step_index)
    packed = 0
    for position in range(1, n):
        diff = int(samples[position]) - predictor
        nibble = 8 if diff < 0 else 0
        magnitude = -diff if diff < 0 else diff
        step = STEP_TABLE[index]
        # 逐位判定：与解码器的「加法重建」互为逆运算（先大后小的贪心分解）。
        if magnitude >= step:
            nibble |= 4
            magnitude -= step
        if magnitude >= (step >> 1):
            nibble |= 2
            magnitude -= (step >> 1)
        if magnitude >= (step >> 2):
            nibble |= 1
        predictor, index = decode_nibble(nibble, predictor, index)
        # 先高后低：奇数位置的下标落在高 4 位。
        if (position - 1) & 1:
            packed |= nibble
            out.append(packed)
            packed = 0
        else:
            packed = nibble << 4
    if (n - 1) & 1:
        out.append(packed)
    return bytes(out)


def decode(data, sample_count):
    """按 C 解码器同样的步进方式解出 sample_count 个样本。"""
    if sample_count == 0:
        return []
    predictor = struct.unpack_from("<h", data, 0)[0]
    index = clamp_step_index(data[2])
    out = [predictor]
    for position in range(1, sample_count):
        packed = data[HEADER_BYTES + ((position - 1) >> 1)]
        nibble = (packed & 0x0F) if ((position - 1) & 1) else (packed >> 4)
        predictor, index = decode_nibble(nibble, predictor, index)
        out.append(predictor)
    return out


def choose_start_index(samples, window=4096, score=None):
    """在前 window 个样本里试几个起始步长索引，选重建误差最小的。

    为什么值得搜：语音的起始斜率差异很大，索引给小了会「追不上」（削平音头，
    听感就是发闷），给大了会把细节量化没了。搜索只看片段开头，代价是一次性的，
    而每条片段都受益。

    score 限制「只用前 score 个样本打分」。固定向量里那 16 个满幅方波跳变是刻意
    造的极端斜率，任何起始索引都追不上，误差与候选无关；让它参与打分会把搜索变成
    噪声（实测会把最佳索引从 60 挑成 20）。真实素材不需要这个参数。
    """
    if len(samples) == 0:
        return 0
    head = samples[:window]
    if score is not None:
        head = head[:score]
    candidates = range(0, STEP_INDEX_MAX + 1, 4)
    best = (None, candidates[0])
    for candidate in candidates:
        encoded = encode(head, candidate)
        rebuilt = decode(encoded, len(head))
        error = 0
        for actual, got in zip(head, rebuilt):
            delta = actual - got
            if delta < 0:
                delta = -delta
            if delta > error:
                error = delta
        if best[0] is None or error < best[0]:
            best = (error, candidate)
    return best[1]
