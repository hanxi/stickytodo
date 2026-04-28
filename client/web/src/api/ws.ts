// 浏览器端 WebSocket 客户端单例：负责与 /api/ws 建立并维持一条长连接，
// 把服务端推送的事件分发给订阅者（典型消费方是 hooks/useRealtimeSync）。
//
// 设计要点：
//   1. 首帧 auth 协议：wss 握手成功后立刻发送 {"type":"auth","token":...}，
//      服务端校验通过会回 {"type":"ready",...}，之后才开始推业务事件；
//      auth 失败或未在 2s 内发送，服务端以 close code 4401 断开。
//   2. 指数退避 + 可见性触发：断线后按 1/2/4/8/16/30s 重试；
//      tab 从隐藏切回可见时，如果已断线会立刻重连（不等退避定时器），
//      以便用户切回前台时立刻能看到最新数据。
//   3. close code 4401 特殊处理：不重连，并 emit "unauthorized" 让上层登出；
//      其他 close code 走正常重连链路。
//   4. 显式 disconnect 语义：用户登出时必须能主动断开并**不再自动重连**，
//      通过 disconnectedByUser 标志区分"用户主动断"和"网络异常断"。
//
// 不做的事情（避免过度设计）：
//   - 事件缓冲 / 断线期补偿：重连成功后 emit "reconnected"，由 React Query
//     invalidate 触发全量重拉，不做客户端级的事件 replay
//   - 消息上行：除首帧 auth 外不会发任何业务消息，后端契约就是纯推送

/** WebSocket 服务端推送的事件帧。data / id 字段视事件类型不同二选一。 */
export interface WSEvent {
  type: string;
  data?: unknown;
  id?: unknown;
}

export type StickyWSEventType = string;

/** 业务事件回调：每条从服务端推来的事件都会回调所有订阅者。 */
export type WSEventHandler = (event: WSEvent) => void;

/**
 * 连接生命周期信号。不通过 WSEventHandler 发送，避免业务代码误处理内部信号。
 *   - 'ready': 已完成首帧 auth，ready 帧收到，可以开始消费业务事件
 *   - 'reconnected': 断线重连成功（只在至少断过一次后触发，首次连接只 emit 'ready'）
 *   - 'unauthorized': 收到 close code 4401，token 失效
 *   - 'disconnected': 连接断开（不论原因），可用于 UI 指示"离线"状态
 */
export type WSSignal = 'ready' | 'reconnected' | 'unauthorized' | 'disconnected';
export type WSSignalHandler = (signal: WSSignal) => void;

// 指数退避的时间表（毫秒）。走到数组末尾后按最后一个值恒定延迟。
const BACKOFF_MS = [1_000, 2_000, 4_000, 8_000, 16_000, 30_000] as const;

class StickyWSClient {
  private ws: WebSocket | null = null;
  private token: string | null = null;
  private baseURL: string | null = null;

  /** 本次会话是否经历过"断线后重连"（用于区分首次 ready vs reconnected 信号）。 */
  private hasBeenDisconnected = false;

  /** 当前处于第几次重试（用于索引 BACKOFF_MS）。成功连接后归零。 */
  private retryAttempt = 0;

  /** 下一次自动重连的 setTimeout 句柄，disconnect 时用于取消。 */
  private reconnectTimer: ReturnType<typeof setTimeout> | null = null;

  /** 用户显式 disconnect 时置 true，所有自动重连路径都会检查它。 */
  private disconnectedByUser = false;

  private readonly eventHandlers = new Set<WSEventHandler>();
  private readonly signalHandlers = new Set<WSSignalHandler>();

  /** 标记 visibilitychange 是否已经注册，避免重复 addEventListener。 */
  private visibilityListenerInstalled = false;

  /**
   * 建立连接。
   *
   * 幂等：
   *   - 若已处于 OPEN 或 CONNECTING 且 token 未变，直接返回（不重建连接）
   *   - token 变化 → 先关旧连接再新建
   *
   * baseURL 可不传：默认用 window.location.origin（同源）；显式传值主要供测试 /
   * 部署到不同域的场景使用。
   */
  connect(token: string, baseURL?: string): void {
    if (!token) {
      // 无 token 等同于"别连"；上层 useRealtimeSync 会在登录后再调 connect
      return;
    }

    this.disconnectedByUser = false;
    this.ensureVisibilityListener();

    const nextBaseURL = baseURL ?? window.location.origin;

    // 已有连接且参数未变 → 不动
    if (
      this.ws &&
      (this.ws.readyState === WebSocket.OPEN ||
        this.ws.readyState === WebSocket.CONNECTING) &&
      this.token === token &&
      this.baseURL === nextBaseURL
    ) {
      return;
    }

    // 参数变化或无连接 → 关闭现有连接，重新拨号
    this.cleanupSocket();
    this.token = token;
    this.baseURL = nextBaseURL;

    this.openOnce();
  }

  /**
   * 主动断开连接；不会再触发自动重连。
   * 调用后再调用 connect() 仍可恢复。
   */
  disconnect(): void {
    this.disconnectedByUser = true;
    if (this.reconnectTimer !== null) {
      clearTimeout(this.reconnectTimer);
      this.reconnectTimer = null;
    }
    this.cleanupSocket();
    this.token = null;
    this.baseURL = null;
    this.retryAttempt = 0;
    this.hasBeenDisconnected = false;
  }

  isConnected(): boolean {
    return this.ws !== null && this.ws.readyState === WebSocket.OPEN;
  }

  /** 注册业务事件订阅器；返回取消订阅的函数以便 React useEffect cleanup。 */
  onEvent(handler: WSEventHandler): () => void {
    this.eventHandlers.add(handler);
    return () => this.eventHandlers.delete(handler);
  }

  /** 注册连接生命周期信号订阅器。 */
  onSignal(handler: WSSignalHandler): () => void {
    this.signalHandlers.add(handler);
    return () => this.signalHandlers.delete(handler);
  }

  // ---------- 内部 ----------

  /** 开一个连接；出错/关闭时的自动重连由 onclose handler 驱动。 */
  private openOnce(): void {
    if (!this.token || !this.baseURL) return;

    const wsURL = toWSURL(this.baseURL);
    let ws: WebSocket;
    try {
      ws = new WebSocket(wsURL);
    } catch (err) {
      // WebSocket 构造函数仅在 URL 非法等极端情况同步抛错——此时没有后续的 onclose 可用，
      // 显式安排一次退避重连，避免连接链路彻底死掉。
      // eslint-disable-next-line no-console
      console.warn('[ws] new WebSocket failed:', err);
      this.scheduleReconnect();
      return;
    }
    this.ws = ws;

    ws.onopen = () => {
      // 握手完成后立即发首帧 auth
      const token = this.token;
      if (!token) {
        // 理论上不会走到：disconnect() 会清 token 并 close，ws 也会随之关
        ws.close();
        return;
      }
      try {
        ws.send(JSON.stringify({ type: 'auth', token }));
      } catch (err) {
        // 发送失败 → close 触发重连
        // eslint-disable-next-line no-console
        console.warn('[ws] send auth failed:', err);
        ws.close();
      }
    };

    ws.onmessage = (ev) => {
      this.handleMessage(ev.data);
    };

    ws.onerror = () => {
      // 浏览器出于安全考虑不暴露错误细节；关闭时 onclose 会被紧接着调用，
      // 自动重连逻辑统一放在 onclose 中，此处只做空置处理，避免干扰。
    };

    ws.onclose = (ev) => {
      // 关闭可能来自：网络异常、服务端主动断、auth 失败 4401、用户 disconnect 等
      this.ws = null;
      this.emitSignal('disconnected');

      if (this.disconnectedByUser) {
        // 用户显式断开，不再重连
        return;
      }

      if (ev.code === 4401) {
        // token 非法/未提供：不再重连，通知上层登出
        this.emitSignal('unauthorized');
        return;
      }

      // 任意其他关闭原因 → 走指数退避
      this.hasBeenDisconnected = true;
      this.scheduleReconnect();
    };
  }

  /** 处理一帧来自服务端的消息。格式非法/非对象直接忽略并打点，不让业务异常传染下来。 */
  private handleMessage(raw: unknown): void {
    if (typeof raw !== 'string') {
      // 业务契约：服务端只会发 TextMessage；收到 Blob/ArrayBuffer 属于协议异常，忽略即可
      return;
    }
    let parsed: unknown;
    try {
      parsed = JSON.parse(raw);
    } catch {
      // eslint-disable-next-line no-console
      console.warn('[ws] drop unparsable frame:', raw);
      return;
    }
    if (!parsed || typeof parsed !== 'object') {
      return;
    }
    const env = parsed as WSEvent;
    if (typeof env.type !== 'string' || env.type.length === 0) {
      return;
    }

    // ready 帧属于协议级信号，不派发给业务订阅者
    if (env.type === 'ready') {
      this.retryAttempt = 0; // 成功 ready 视为连接稳定，重置退避计数
      this.emitSignal(this.hasBeenDisconnected ? 'reconnected' : 'ready');
      return;
    }

    // 业务事件：复制一遍订阅集合后再遍历，避免遍历过程中被 unsubscribe 改变
    const snapshot = Array.from(this.eventHandlers);
    for (const h of snapshot) {
      try {
        h(env);
      } catch (err) {
        // eslint-disable-next-line no-console
        console.error('[ws] event handler threw:', err);
      }
    }
  }

  private emitSignal(signal: WSSignal): void {
    const snapshot = Array.from(this.signalHandlers);
    for (const h of snapshot) {
      try {
        h(signal);
      } catch (err) {
        // eslint-disable-next-line no-console
        console.error('[ws] signal handler threw:', err);
      }
    }
  }

  /** 安排下一次重连，按当前 retryAttempt 索引 BACKOFF_MS。 */
  private scheduleReconnect(): void {
    if (this.disconnectedByUser) return;
    if (!this.token || !this.baseURL) return;
    if (this.reconnectTimer !== null) return;

    const idx = Math.min(this.retryAttempt, BACKOFF_MS.length - 1);
    const delay = BACKOFF_MS[idx]!;
    this.retryAttempt += 1;

    this.reconnectTimer = setTimeout(() => {
      this.reconnectTimer = null;
      this.openOnce();
    }, delay);
  }

  /** 把当前 WebSocket 清理掉（移除 handler + close），不触发业务信号。 */
  private cleanupSocket(): void {
    const ws = this.ws;
    if (!ws) return;
    // 清掉 handler 避免 close 事件回调干扰下一次连接的状态机
    ws.onopen = null;
    ws.onclose = null;
    ws.onerror = null;
    ws.onmessage = null;
    if (
      ws.readyState === WebSocket.OPEN ||
      ws.readyState === WebSocket.CONNECTING
    ) {
      try {
        ws.close();
      } catch {
        // close 可能抛（极少见），忽略
      }
    }
    this.ws = null;
  }

  /**
   * 注册 visibilitychange：tab 从隐藏切回可见且已断线时，绕过退避定时器立即重连。
   *
   * 只在"浏览器长期隐藏后被唤醒，正好处于退避间隔中"这种场景见效——普通在线时
   * 已有 ws 连接正常收 ping/pong 维持，不走此分支。
   * SSR / 非浏览器环境没有 document 对象，跳过注册。
   */
  private ensureVisibilityListener(): void {
    if (this.visibilityListenerInstalled) return;
    if (typeof document === 'undefined') return;

    document.addEventListener('visibilitychange', () => {
      if (document.visibilityState !== 'visible') return;
      if (this.disconnectedByUser) return;
      if (this.ws && this.ws.readyState === WebSocket.OPEN) return;
      if (!this.token || !this.baseURL) return;

      // 立即重连：清掉 pending 退避定时器、重置退避计数
      if (this.reconnectTimer !== null) {
        clearTimeout(this.reconnectTimer);
        this.reconnectTimer = null;
      }
      this.retryAttempt = 0;
      this.openOnce();
    });
    this.visibilityListenerInstalled = true;
  }
}

/**
 * 把 http(s)://host/path 形式的 base URL 转换成 ws(s)://host/api/ws。
 * 显式写在这里而不是用 URL 对象拼接，是为了保留清晰的 ws/http 对应关系，
 * 读代码时不需要在心里模拟 URL 构造器的行为。
 */
function toWSURL(baseURL: string): string {
  let prefix: string;
  if (baseURL.startsWith('https://')) {
    prefix = 'wss://' + baseURL.slice('https://'.length);
  } else if (baseURL.startsWith('http://')) {
    prefix = 'ws://' + baseURL.slice('http://'.length);
  } else {
    // 非 http(s) 前缀：极少见（如 file://）。此时直接拼，让 WebSocket 构造器抛错交给上层处理
    prefix = baseURL;
  }
  // 去掉末尾 '/' 再挂 /api/ws，避免出现 "//api/ws" 路径
  if (prefix.endsWith('/')) {
    prefix = prefix.slice(0, -1);
  }
  return `${prefix}/api/ws`;
}

/**
 * StickyWS 单例。整个 Web App 共用一条连接，避免多标签页外的多连接浪费。
 * （多个浏览器 tab 会各自建立独立连接，这是预期行为。）
 */
export const stickyWS = new StickyWSClient();
