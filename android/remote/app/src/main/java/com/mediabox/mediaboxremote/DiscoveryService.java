package com.mediabox.mediaboxremote;

import android.app.Service;
import android.content.Intent;
import android.os.Binder;
import android.os.Handler;
import android.os.IBinder;
import android.util.Log;
import android.widget.Toast;

import java.net.DatagramPacket;
import java.net.DatagramSocket;
import java.net.InetAddress;
import java.net.UnknownHostException;
import java.util.HashMap;
import java.util.Iterator;
import java.util.Map;
import java.util.concurrent.RunnableFuture;

public class DiscoveryService extends Service
{
    private static final int FIELD_INDEX_ID = 1;
    private static final int FIELD_INDEX_NAME = 2;
    private static final int FIELD_INDEX_ADDRESS = 3;
    private static final int FIELD_INDEX_FEATURES = 4;
    private static final int ANNOUNCEMENTS_PORT = 49550;

    private DatagramSocket socket = null;
    private Handler main_thread = null;
    private final ServiceBinder binder = new ServiceBinder();
    private final Object device_list_lock = new Object();
    private final Map<String, Device> device_list = new HashMap<>();
    private final Thread gc_thread = new Thread(new Runnable()
    {
        @Override
        public void run()
        {
            while (!Thread.interrupted())
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
        }
    });

    private final Thread listener_thread = new Thread(new Runnable()
    {
        @Override
        public void run()
        {
            while (!Thread.interrupted())
            {
                try
                {
                    byte[] buf = new byte[15000];
                    if (socket == null || socket.isClosed())
                    {
                        socket = new DatagramSocket(ANNOUNCEMENTS_PORT);
                        socket.setBroadcast(true);
                    }

                    DatagramPacket packet = new DatagramPacket(buf, buf.length);
                    socket.receive(packet);

                    processMessage(packet.getAddress().getHostAddress(),
                            new String(packet.getData()).trim());
                }
                catch (final Exception ex)
                {
                    Log.d("DiscoveryService", ex.toString());
                    try
                    {
                        main_thread.post(new Runnable() {
                             @Override
                             public void run() {
                                 Toast.makeText(DiscoveryService.this,
                                         String.format("Listen %s", ex.getMessage()),
                                         Toast.LENGTH_LONG).show();
                             }
                         });
                        Thread.sleep(10 * 1000L);
                    }
                    catch (InterruptedException e)
                    {
                        /* nothing */
                    }
                }
                finally
                {
                    if (socket != null && !socket.isClosed())
                    {
                        socket.close();
                    }
                }
            }
        }
    });

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

        /* if the device does not not support the player
           feature then it is not a set-top box and it doesn't
           need a remote. */
        if (!fields[FIELD_INDEX_FEATURES].contains("PLAYER")) {
            return;
        }

        synchronized (device_list_lock)
        {
            Device dev;

            if (device_list.containsKey(fields[FIELD_INDEX_ID]))
            {
                dev = device_list.get(fields[FIELD_INDEX_ID]);
            }
            else
            {
                dev = new Device();
                device_list.put(fields[FIELD_INDEX_ID], dev);
                Log.d("DiscoveryService", String.format("Host %s (%s) added.",
                        fields[FIELD_INDEX_ID], fields[FIELD_INDEX_ADDRESS]));
            }

            dev.id = fields[FIELD_INDEX_ID];
            dev.name = fields[FIELD_INDEX_NAME];
            dev.address = fields[FIELD_INDEX_ADDRESS];
            dev.timestamp = System.currentTimeMillis() / 1000L;
        }

        Log.d("DiscoveryService", String.format("Host %s (%s) updated.",
                fields[FIELD_INDEX_ID], fields[FIELD_INDEX_ADDRESS]));
    }

    @Override
    public void onCreate()
    {
        super.onCreate();
        this.main_thread = new Handler();
        this.listener_thread.start();
        this.gc_thread.start();
    }

    @Override
    public void onDestroy()
    {
        super.onDestroy();
        try
        {
            listener_thread.interrupt();
            gc_thread.interrupt();
            listener_thread.join();
            gc_thread.join();
        }
        catch (InterruptedException ex)
        {
            /* nothing */
        }
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

