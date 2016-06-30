package com.mediabox.mediaboxremote;

import android.app.Service;
import android.content.Intent;
import android.os.IBinder;
import android.util.Log;

import java.net.DatagramPacket;
import java.net.DatagramSocket;
import java.net.InetAddress;

public class UDPListener extends Service
{
    private Thread UDPListenerThread = null;
    private DatagramSocket socket = null;
    private int ANNOUNCEMENTS_PORT = 49550;

    public UDPListener()
    {
    }

    private void processMessage(String src, String msg)
    {
        String[] fields;
        String hostId;
        String hostIp;
        String features;

        /* ignore messages that don't start with MediaBox: */
        if (!msg.startsWith("MediaBox:"))
        {
            return;
        }

        Log.d("UDPListener", String.format("%s: %s", src, msg));

        fields = msg.split(":");

        if (fields.length < 4) {
            return;
        }

        hostId = fields[1];
        hostIp = fields[2];
        features = fields[3];

        if (!features.contains("PLAYER")) {
            return;
        }

        /* TODO: Make this thread safe since a background
           thread will have to remove stale entries periodically
         */
        if (Globals.DeviceList.containsKey(hostId))
        {
            if (Globals.DeviceList.get(hostId).equals(hostIp))
            {
                /* item is already on list */
                return;
            }
            else
            {
                Globals.DeviceList.remove(hostId);
                Globals.DeviceList.put(hostId, hostIp);
                Log.d("UDPListener", String.format("Host %s (%s) updated.",
                        hostId, hostIp));
            }
        }
        else
        {
            Globals.DeviceList.put(hostId, hostIp);
            Log.d("UDPListener", String.format("Host %s (%s) added to list.",
                    hostId, hostIp));
        }
    }

    @Override
    public int onStartCommand(Intent intent, int flags, int startId)
    {
        final int sid = startId;

        assert UDPListenerThread == null;

        UDPListenerThread = new Thread(new Runnable()
        {
            @Override
            public void run()
            {
                try
                {
                    InetAddress ip = InetAddress.getByName("255.255.255.255");

                    while (true)
                    {
                        byte[] buf = new byte[15000];
                        if (socket == null || socket.isClosed())
                        {
                            socket = new DatagramSocket(ANNOUNCEMENTS_PORT, ip);
                            socket.setBroadcast(true);
                        }

                        DatagramPacket packet = new DatagramPacket(buf, buf.length);
                        socket.receive(packet);

                        processMessage(packet.getAddress().getHostAddress(),
                                new String(packet.getData()).trim());
                    }
                }
                catch (Exception ex)
                {
                    Log.d("UDPListener", ex.toString());
                }
                finally
                {
                    if (socket != null && !socket.isClosed())
                    {
                        socket.close();
                    }
                    stopSelf(sid);
                    UDPListenerThread = null;
                }
            }
        });
        UDPListenerThread.start();
        return START_STICKY;
    }

    @Override
    public void onDestroy()
    {
        try
        {
            UDPListenerThread.interrupt();
            UDPListenerThread.join();
        }
        catch (InterruptedException ex)
        {
            /* nothing */
        }
        super.onDestroy();
    }

    @Override
    public IBinder onBind(Intent intent)
    {
        throw new UnsupportedOperationException("Not supported");
    }
}
