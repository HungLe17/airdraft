// SPDX-License-Identifier: GPL-2.0-only
using System.Net;
using System.Net.Sockets;
using System.Runtime.InteropServices;

namespace Airdraft.Service;

/// <summary>
/// Sends an ICMP echo using an explicit source address from the adapter under observation.
/// </summary>
static class Icmp
{
    [DllImport("iphlpapi.dll", SetLastError = true)]
    static extern IntPtr IcmpCreateFile();

    [DllImport("iphlpapi.dll", SetLastError = true)]
    static extern IntPtr Icmp6CreateFile();

    [DllImport("iphlpapi.dll", SetLastError = true)]
    static extern bool IcmpCloseHandle(IntPtr handle);

    [DllImport("iphlpapi.dll", SetLastError = true)]
    static extern uint IcmpSendEcho2Ex(IntPtr handle, IntPtr evt, IntPtr apc, IntPtr apcContext, uint source,
                                       uint destination, byte[] data, ushort size, IntPtr options, byte[] reply,
                                       uint replySize, uint timeout);

    [DllImport("iphlpapi.dll", SetLastError = true)]
    static extern uint Icmp6SendEcho2(IntPtr handle, IntPtr evt, IntPtr apc, IntPtr apcContext,
                                     in SockaddrIn6 source, in SockaddrIn6 destination, byte[] data,
                                     ushort size, IntPtr options, [Out] byte[] reply, uint replySize, uint timeout);

    [StructLayout(LayoutKind.Sequential)]
    unsafe struct SockaddrIn6
    {
        public ushort Family;
        public ushort Port;
        public uint FlowInfo;
        public fixed byte Address[16];
        public uint ScopeId;

        public static SockaddrIn6 From(IPAddress address)
        {
            var socket = new SockaddrIn6 { Family = (ushort)AddressFamily.InterNetworkV6, ScopeId = (uint)address.ScopeId };
            address.GetAddressBytes().CopyTo(new Span<byte>(socket.Address, 16));
            return socket;
        }
    }

    static uint Addr(IPAddress a) => BitConverter.ToUInt32(a.GetAddressBytes(), 0);

    /// <summary>True when `destination` answered within `timeoutMs`, from `source`.</summary>
    public static bool Ping(IPAddress source, IPAddress destination, uint timeoutMs)
    {
        var ipv6 = source.AddressFamily == AddressFamily.InterNetworkV6;
        var h = ipv6 ? Icmp6CreateFile() : IcmpCreateFile();
        if (h == IntPtr.Zero || h == new IntPtr(-1)) return false;
        try
        {
            var data = new byte[32];
            var reply = new byte[256]; /* Echo reply, request data and error overhead. */
            if (ipv6)
            {
                var from = SockaddrIn6.From(source);
                var to = SockaddrIn6.From(destination);
                var replies = Icmp6SendEcho2(h, IntPtr.Zero, IntPtr.Zero, IntPtr.Zero, in from, in to, data,
                                            (ushort)data.Length, IntPtr.Zero, reply, (uint)reply.Length, timeoutMs);
                // ICMPV6_ECHO_REPLY puts the address in packed IPV6_ADDRESS_EX bytes 6–21.
                // The Windows SDK aligns the following IP_STATUS field to byte 28.
                return replies != 0 && BitConverter.ToUInt32(reply, 28) == 0 &&
                    reply.AsSpan(6, 16).SequenceEqual(destination.GetAddressBytes());
            }
            var n = IcmpSendEcho2Ex(h, IntPtr.Zero, IntPtr.Zero, IntPtr.Zero, Addr(source), Addr(destination), data,
                                    (ushort)data.Length, IntPtr.Zero, reply, (uint)reply.Length, timeoutMs);
            if (n == 0) return false;
            // ICMP_ECHO_REPLY: Address (4), Status (4): IP_SUCCESS is 0.
            return BitConverter.ToUInt32(reply, 4) == 0 && BitConverter.ToUInt32(reply, 0) == Addr(destination);
        }
        finally { IcmpCloseHandle(h); }
    }
}
