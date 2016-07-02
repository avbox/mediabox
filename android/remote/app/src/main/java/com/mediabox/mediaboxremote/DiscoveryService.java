package com.mediabox.mediaboxremote;

import android.app.Service;
import android.content.Intent;
import android.os.Binder;
import android.os.IBinder;
import android.util.Log;

import java.net.DatagramPacket;
import java.net.DatagramSocket;
import java.net.InetAddress;
import java.util.HashMap;
import java.util.Iterator;
import java.util.Map;

public class DiscoveryService extends Service
{
    private Thread listener_thread = null;
    private Thread gc_thread = null;
    private DatagramSocket socket = null;
    private final ServiceBinder binder = new ServiceBinder();
    private int ANNOUNCEMENTS_PORT = 49550;
    private final Object device_list_lock = new Object();
    private Map<String, Device> device_list = new HashMap<>();

    public DiscoveryService()
    {
    }

    public Device[] getDeviceList()
    {
        return this.device_list.values().toArray((Device[])
                java.lang.reflect.Array.newInstance(Device.class, device_list.size()));
    }

    private void processMessage(String src, String msg)
    {
        String[] fields;
        String hostId;
        String hostName;
        String hostIp;
        String features;
        Device dev;

        /* ignore messages that don't start with MediaBox: */
        if (!msg.startsWith("MediaBox:"))
        {
            return;
        }

        Log.d("DiscoveryService", String.format("%s: %s", src, msg));

        fields = msg.split(":");

        if (fields.length < 5) {
            return;
        }

        hostId = fields[1];
        hostName = fields[2];
        hostIp = fields[3];
        features = fields[4];

        /* if the device does not not support the player
           feature then it is not a set-top box and it doesn't
           need a remote. */
        if (!features.contains("PLAYER")) {
            return;
        }

        synchronized (device_list_lock)
        {
            if (device_list.containsKey(hostId))
            {
                dev = device_list.get(hostId);
            }
            else
            {
                dev = new Device();
                device_list.put(hostId, dev);
                Log.d("DiscoveryService", String.format("Host %s (%s) added.",
                        hostId, hostIp));
            }

            dev.address = hostIp;
            dev.id = hostId;
            dev.name = hostName;
            dev.timestamp = System.currentTimeMillis() / 1000L;
        }
        Log.d("DiscoveryService", String.format("Host %s (%s) updated.",
                hostId, hostIp));
    }

    @Override
    public void onCreate()
    {
        assert gc_thread == null;
        assert listener_thread == null;

        gc_thread = new Thread(new Runnable()
        {
            @Override
            public void run()
            {
                while (true)
                {
                    long expired = (System.currentTimeMillis() / 1000L) - 15;

                    try
                    {
                        Thread.sleep(10 * 1000L);
                    }
                    catch (InterruptedException ex)
                    {
                        break;
                    }

                    Log.d("DiscoveryService", "Removing expired entries.");

                    synchronized (device_list_lock)
                    {
                        Iterator<Device> iter = device_list.values().iterator();
                        while (iter.hasNext())
                        {
                            Device dev = iter.next();
                            if (dev.timestamp < expired)
                            {
                                iter.remove();
                                Log.d("DiscoveryService",
                                        String.format("Timestamp %d, exp %d. Removing %s",
                                        dev.timestamp, expired, dev.name));
                            }
                        }
                    }
                }
                gc_thread = null;
            }
        });

        listener_thread = new Thread(new Runnable()
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
                    Log.d("DiscoveryService", ex.toString());
                }
                finally
                {
                    if (socket != null && !socket.isClosed())
                    {
                        socket.close();
                    }
                    listener_thread = null;
                }
            }
        });

        listener_thread.start();
        gc_thread.start();
        super.onCreate();
    }

    @Override
    public void onDestroy()
    {
        try
        {
            listener_thread.interrupt();
            listener_thread.join();
        }
        catch (InterruptedException ex)
        {
            /* nothing */
        }
        try
        {
            gc_thread.interrupt();
            gc_thread.join();
        }
        catch (InterruptedException ex)
        {
            /* nothing */
        }
        super.onDestroy();
    }

    @Override
    public int onStartCommand(Intent intent, int flags, int startId)
    {
        return super.onStartCommand(intent, flags, startId);
    }

    @Override
    public IBinder onBind(Intent intent)
    {
        return binder;
    }

    public class ServiceBinder extends Binder
    {
        DiscoveryService getService()
        {
            return DiscoveryService.this;
        }
    }
}

