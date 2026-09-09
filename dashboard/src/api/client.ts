import type { Peer, PeerDetail, TimeSeriesPoint, GpuJob, DaemonConfig, LogEntry, LocalStatus } from './types';

const API_BASE = '/api'; // Handled by MSW in dev, relative in prod if hosted by bridge

async function fetcher<T>(url: string, options?: RequestInit): Promise<T> {
  const response = await fetch(`${API_BASE}${url}`, options);
  if (!response.ok) {
    throw new Error(`API Error: ${response.status} ${response.statusText}`);
  }
  return response.json();
}

export const apiClient = {
  getPeers: () => fetcher<Peer[]>('/peers'),
  getPeer: (id: string) => fetcher<PeerDetail>(`/peers/${id}`),
  getPeerHistory: (id: string) => fetcher<TimeSeriesPoint[]>(`/peers/${id}/history`),
  getGpuJobs: () => fetcher<GpuJob[]>('/gpu/jobs'),
  getConfig: () => fetcher<DaemonConfig>('/config'),
  updateConfig: (config: DaemonConfig) => fetcher<DaemonConfig>('/config', {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify(config),
  }),
  joinPool: () => fetcher<{ success: boolean }>('/pool/join', { method: 'POST' }),
  leavePool: () => fetcher<{ success: boolean }>('/pool/leave', { method: 'POST' }),
  getLogs: (severity?: string, search?: string, page = 1) => {
    const params = new URLSearchParams();
    if (severity) params.set('severity', severity);
    if (search) params.set('search', search);
    params.set('page', page.toString());
    return fetcher<LogEntry[]>(`/logs?${params.toString()}`);
  },
  getLocalStatus: () => fetcher<LocalStatus>('/status/local'),
};
