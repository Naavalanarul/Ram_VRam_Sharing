import { useParams } from 'react-router-dom';
import { useQuery } from '@tanstack/react-query';
import { apiClient } from '../api/client';
import { LineChart, Line, XAxis, YAxis, Tooltip, ResponsiveContainer } from 'recharts';

function formatBytes(bytes: number) {
  if (bytes === 0) return '0 GB';
  const gb = bytes / (1024 ** 3);
  return `${gb.toFixed(1)} GB`;
}

export function PeerDetail() {
  const { id } = useParams<{ id: string }>();

  const { data: peer, isLoading } = useQuery({
    queryKey: ['peer', id],
    queryFn: () => apiClient.getPeer(id!),
    enabled: !!id,
    refetchInterval: 5000,
  });

  const { data: history = [] } = useQuery({
    queryKey: ['peer-history', id],
    queryFn: () => apiClient.getPeerHistory(id!),
    enabled: !!id,
    refetchInterval: 5000,
  });

  if (isLoading) return <div className="text-[var(--muted-foreground)] font-mono">Loading peer details...</div>;
  if (!peer) return <div className="text-[var(--critical)] font-mono">Peer not found.</div>;

  const chartData = history.map(p => ({
    time: new Date(p.timestamp).toLocaleTimeString([], { hour: '2-digit', minute: '2-digit' }),
    ram: p.ramUsageBytes / (1024 ** 3),
    vram: p.vramUsageBytes / (1024 ** 3),
  }));

  return (
    <div className="flex flex-col gap-6">
      <div className="flex justify-between items-start">
        <div>
          <h1 className="text-2xl font-semibold font-mono">{peer.hostname}</h1>
          <div className="text-[var(--muted-foreground)] font-mono text-xs mt-1">ID: {peer.id} &bull; {peer.address}</div>
        </div>
        <div className="border border-[var(--healthy)] bg-[var(--healthy-bg)] text-[var(--healthy)] px-2 py-1 rounded-[3px] text-[10px] font-mono">
          {peer.state}
        </div>
      </div>

      <div className="grid grid-cols-1 lg:grid-cols-2 gap-6">
        <div className="bg-[var(--card)] border border-[var(--border)] rounded p-4">
          <h3 className="text-[11px] font-semibold text-[var(--muted-foreground)] uppercase tracking-widest mb-4">Memory Pressure</h3>
          <div className="h-64">
            <ResponsiveContainer width="100%" height="100%">
              <LineChart data={chartData}>
                <XAxis dataKey="time" stroke="var(--border)" tick={{ fill: 'var(--muted-foreground)', fontSize: 10, fontFamily: 'var(--font-mono)' }} />
                <YAxis stroke="var(--border)" tick={{ fill: 'var(--muted-foreground)', fontSize: 10, fontFamily: 'var(--font-mono)' }} />
                <Tooltip 
                  contentStyle={{ backgroundColor: 'var(--card)', borderColor: 'var(--border-subtle)', borderRadius: '4px' }}
                  itemStyle={{ fontFamily: 'var(--font-mono)', fontSize: '11px' }}
                  labelStyle={{ fontFamily: 'var(--font-mono)', fontSize: '11px', color: 'var(--muted-foreground)' }}
                />
                <Line type="monotone" dataKey="ram" name="RAM (GB)" stroke="var(--primary)" strokeWidth={2} dot={false} isAnimationActive={false} />
                <Line type="monotone" dataKey="vram" name="VRAM (GB)" stroke="var(--compute)" strokeWidth={2} dot={false} isAnimationActive={false} />
              </LineChart>
            </ResponsiveContainer>
          </div>
        </div>

        <div className="flex flex-col gap-4">
          <div className="bg-[var(--card)] border border-[var(--border)] rounded flex flex-col">
            <div className="p-3 border-b border-[var(--border-subtle)] text-[11px] font-semibold text-[var(--muted-foreground)] uppercase tracking-widest">
              Hosted Allocations ({peer.hostedAllocations.length})
            </div>
            <div className="p-0">
              {peer.hostedAllocations.map(a => (
                <div key={a.id} className="flex justify-between p-3 border-b border-[var(--border-subtle)] last:border-0 hover:bg-[#161b22]">
                  <span className="font-mono text-xs">{a.ownerPeerId}</span>
                  <span className="font-mono text-xs text-[var(--primary)]">{formatBytes(a.sizeBytes)}</span>
                </div>
              ))}
              {peer.hostedAllocations.length === 0 && (
                <div className="p-4 text-xs font-mono text-[var(--muted-foreground)]">No hosted allocations.</div>
              )}
            </div>
          </div>
          
          <div className="bg-[var(--card)] border border-[var(--border)] rounded flex flex-col">
            <div className="p-3 border-b border-[var(--border-subtle)] text-[11px] font-semibold text-[var(--muted-foreground)] uppercase tracking-widest">
              Placed Allocations ({peer.placedAllocations.length})
            </div>
            <div className="p-0">
              {peer.placedAllocations.length === 0 && (
                <div className="p-4 text-xs font-mono text-[var(--muted-foreground)]">No placed allocations.</div>
              )}
            </div>
          </div>
        </div>
      </div>
    </div>
  );
}
