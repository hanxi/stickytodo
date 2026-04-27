// 色彩工具 —— 从 macOS 客户端 CodableRGBA.swift 移植。
// 用于根据便签背景色的亮度动态切换前景（优先级小旗）颜色，
// 避免浅色便签上出现几乎看不清的亮红旗、深色便签上出现暗色低对比度。

export interface Rgb {
  r: number;
  g: number;
  b: number;
}

export function hexToRgb(hex: string | null | undefined): Rgb {
  // 防御性处理：老版本 localStorage 里的 sticky 可能缺 color 字段，或者
  // 被外部数据源传进非字符串值。这里统一兜成白色（1,1,1），调用方拿到的
  // luminance = 1 → isLightBackground=true，与默认浅色便签行为一致。
  if (typeof hex !== 'string' || hex.length === 0) {
    return { r: 1, g: 1, b: 1 };
  }
  const cleaned = hex.replace('#', '').trim();
  const value = cleaned.length === 3
    ? cleaned.split('').map((c) => c + c).join('')
    : cleaned;
  const num = Number.parseInt(value, 16);
  if (!Number.isFinite(num) || value.length !== 6) {
    return { r: 1, g: 1, b: 1 };
  }
  return {
    r: ((num >> 16) & 0xff) / 255,
    g: ((num >> 8) & 0xff) / 255,
    b: (num & 0xff) / 255,
  };
}

/** 感官亮度（0..1）。与 macOS 端一致：Rec.709 权重。 */
export function luminance(hex: string | null | undefined): number {
  const { r, g, b } = hexToRgb(hex);
  return 0.2126 * r + 0.7152 * g + 0.0722 * b;
}

export function isLightBackground(hex: string | null | undefined): boolean {
  return luminance(hex) >= 0.6;
}

/**
 * 根据优先级 + 背景色亮度返回旗帜颜色。
 * 浅色背景用深红/橙/深黄；深色背景用亮红/橙/黄。
 */
export function priorityColor(
  priority: number,
  bgHex: string | null | undefined,
): string | null {
  if (priority <= 0) {
    return null;
  }
  const light = isLightBackground(bgHex);
  if (light) {
    if (priority >= 3) return '#A30000';
    if (priority === 2) return '#C04A00';
    return '#8A6F00';
  }
  if (priority >= 3) return '#FF6B6B';
  if (priority === 2) return '#FFA552';
  return '#FFD666';
}

/** 前景文字颜色（供标题/正文使用） */
export function foregroundFor(bgHex: string | null | undefined): string {
  return isLightBackground(bgHex) ? '#1f1f1f' : '#f5f5f5';
}
