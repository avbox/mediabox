package com.mediabox.mediaboxremote;

import android.bluetooth.BluetoothAdapter;
import android.bluetooth.BluetoothDevice;
import android.bluetooth.BluetoothSocket;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.content.SharedPreferences;
import android.graphics.Point;
import android.os.Parcelable;
import android.preference.PreferenceManager;
import android.support.v7.app.AppCompatActivity;
import android.os.Bundle;
import android.util.Log;
import android.view.Display;
import android.view.KeyEvent;
import android.view.Menu;
import android.view.MenuInflater;
import android.view.MenuItem;
import android.view.View;
import android.view.WindowManager;
import android.view.inputmethod.InputMethodManager;
import android.widget.Button;

import java.io.BufferedWriter;
import java.io.IOException;
import java.io.OutputStreamWriter;
import java.io.PrintWriter;
import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Method;
import java.net.InetAddress;
import java.net.Socket;
import java.net.UnknownHostException;
import java.util.Set;

public class RemoteActivity extends AppCompatActivity
{
    private Socket socket = null;
    private BluetoothSocket btsocket = null;
    private static final int SERVER_PORT = 2048;
    private static BluetoothAdapter btdev = BluetoothAdapter.getDefaultAdapter();

    private final BroadcastReceiver mReceiver = new BroadcastReceiver() {
        public void onReceive(Context context, Intent intent)
        {
            String action = intent.getAction();
            if (BluetoothDevice.ACTION_UUID.equals(action))
            {
                BluetoothDevice device = intent.getParcelableExtra(BluetoothDevice.EXTRA_DEVICE);
                Parcelable[] uuids = intent.getParcelableArrayExtra(BluetoothDevice.EXTRA_UUID);
                if (uuids.length > 0)
                {
                    for (Parcelable uuid : uuids)
                    {
                        Log.d("RemoteActivity", String.format("Found %s UUID: %s",
                                device.getName(), uuid.toString()));
                    }
                }
            }
        }
    };

    private void closeSocket()
    {
        if (this.socket != null)
        {
            try
            {
                this.socket.close();
                this.socket = null;
            }
            catch (java.io.IOException e)
            {
                e.printStackTrace();
            }
        }
        if (this.btsocket != null)
        {
            try
            {
                this.btsocket.close();
                this.btsocket = null;
            }
            catch (java.io.IOException e)
            {
                e.printStackTrace();
            }
        }

    }

    private void openSocket()
    {
        closeSocket();
        new Thread(new ClientThread()).start();
    }

    private void sendMessage(String msg)
    {
        try {
            PrintWriter out;
            if (this.socket != null)
            {
                Log.d("RemoteActivity", "Sending via TCP");
                out = new PrintWriter(new BufferedWriter(
                        new OutputStreamWriter(socket.getOutputStream())), true);
            }
            else if (this.btsocket != null)
            {
                Log.d("RemoteActivity", "Sending via Bluetooth");
                out = new PrintWriter(new BufferedWriter(
                        new OutputStreamWriter(btsocket.getOutputStream())), true);
            }
            else
            {
                return;
            }
            out.println(msg);
            out.flush();

        } catch (UnknownHostException e) {
            e.printStackTrace();
        } catch (IOException e) {
            e.printStackTrace();
        } catch (Exception e) {
            e.printStackTrace();
        }
    }


    @Override
    protected void onCreate(Bundle savedInstanceState)
    {
        super.onCreate(savedInstanceState);
        setContentView(R.layout.activity_remote);
        startService(new Intent(this, DiscoveryService.class));
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);

        this.findViewById(R.id.btnKeyboard).setOnKeyListener(new View.OnKeyListener()
        {
            @Override
            public boolean onKey(View v, int keyCode, KeyEvent event)
            {
                if (event.getAction() == KeyEvent.ACTION_UP)
                {
                    if (keyCode == KeyEvent.KEYCODE_DEL)
                    {
                        sendMessage("CLEAR");
                    }
                    else
                    {
                        sendMessage(String.format("KEY:%c",
                                Character.toUpperCase((char) event.getUnicodeChar())));
                    }
                }
                return true;
            }
        });
        IntentFilter filter = new IntentFilter(BluetoothDevice.ACTION_UUID);
        registerReceiver(mReceiver, filter);
    }

    public boolean onCreateOptionsMenu(Menu menu)
    {
        MenuInflater inflater = getMenuInflater();
        inflater.inflate(R.menu.main_menu, menu);
        return true;
    }

    public boolean onOptionsItemSelected(MenuItem item)
    {
        int id = item.getItemId();
        if (id == R.id.discover)
        {
            startActivity(new Intent(this, DeviceListActivity.class));
            return true;
        }
        else if (id == R.id.bluetooth)
        {
            SharedPreferences prefs = PreferenceManager.getDefaultSharedPreferences(this);
            SharedPreferences.Editor editor = prefs.edit();
            editor.putString("device", "00:02:72:13:75:93");
            editor.apply();
            openSocket();
        }
        else if (id == R.id.about)
        {
            startActivity(new Intent(this, AboutActivity.class));
            return true;
        }
        return false;
    }

    @Override
    protected void onDestroy()
    {
        Log.d("Remote", "Destroying");
        unregisterReceiver(mReceiver);
        stopService(new Intent(this, DiscoveryService.class));
        super.onDestroy();
    }

    @Override
    protected void onStart()
    {
        super.onStart();
        this.openSocket();
        Display display = getWindowManager().getDefaultDisplay();
        Point size = new Point();
        display.getSize(size);
        int width = (size.x / 4) - 16;
        ((Button) this.findViewById(R.id.btnPrev)).setMinWidth(width);
        ((Button) this.findViewById(R.id.btnRew)).setMinWidth(width);
        ((Button) this.findViewById(R.id.btnFF)).setMinWidth(width);
        ((Button) this.findViewById(R.id.btnNext)).setMinWidth(width);

        ((Button) this.findViewById(R.id.btnPrev)).setWidth(width);
        ((Button) this.findViewById(R.id.btnRew)).setWidth(width);
        ((Button) this.findViewById(R.id.btnFF)).setWidth(width);
        ((Button) this.findViewById(R.id.btnNext)).setWidth(width);
    }

    @Override
    protected void onStop()
    {
        super.onStop();
        this.closeSocket();
        ((InputMethodManager) this.getSystemService(Context.INPUT_METHOD_SERVICE))
                .hideSoftInputFromWindow(findViewById(R.id.btnKeyboard).getWindowToken(), 0);
    }

    public void onKeyboard(View view) {
        InputMethodManager im = (InputMethodManager) view.getContext()
                .getSystemService(Context.INPUT_METHOD_SERVICE);
        im.showSoftInput(view, InputMethodManager.SHOW_FORCED);
    }

    public void onButtonPressed(View view)
    {
        if (view == this.findViewById(R.id.btnMenu))
        {
            sendMessage("MENU");
        }
        else if (view == this.findViewById(R.id.btnUp))
        {
            sendMessage("UP");
        }
        else if (view == this.findViewById(R.id.btnLeft))
        {
            sendMessage("LEFT");
        }
        else if (view == this.findViewById(R.id.btnRight))
        {
            sendMessage("RIGHT");
        }
        else if (view == this.findViewById(R.id.btnDown))
        {
            sendMessage("DOWN");
        }
        else if (view == this.findViewById(R.id.btnBack))
        {
            sendMessage("BACK");
        }
        else if (view == this.findViewById(R.id.btnEnter))
        {
            sendMessage("ENTER");
        }
        else if (view == this.findViewById(R.id.btnStop))
        {
            sendMessage("STOP");
        }
        else if (view == this.findViewById(R.id.btnPlay))
        {
            sendMessage("PLAY");
        }
        else if (view == this.findViewById(R.id.btnInfo))
        {
            sendMessage("INFO");
        }
        else if (view == this.findViewById(R.id.btnPrev))
        {
            sendMessage("PREV");
        }
        else if (view == this.findViewById(R.id.btnNext))
        {
            sendMessage("NEXT");
        }
        else if (view == this.findViewById(R.id.btnRew))
        {
            sendMessage("RW");
        }
        else if (view == this.findViewById(R.id.btnFF))
        {
            sendMessage("FF");
        }
        else if (view == this.findViewById(R.id.btnVolUp))
        {
            sendMessage("VOLUP");
        }
        else if (view == this.findViewById(R.id.btnVolDown))
        {
            sendMessage("VOLDOWN");
        }
    }


    class ClientThread implements Runnable {
        @Override
        public void run()
        {
            try
            {
                SharedPreferences prefs = PreferenceManager.
                        getDefaultSharedPreferences(RemoteActivity.this);
                if (prefs.getString("device", "").equals("00:02:72:13:75:93"))
                {
                    Log.d("RemoteActivity", "Opening Bluetooth socket");

                    if (btdev != null)
                    {
                        btdev.cancelDiscovery();

                        Set<BluetoothDevice> devices = btdev.getBondedDevices();
                        if (devices.size() > 0) {
                            for (BluetoothDevice dev : devices)
                            {
                                if (dev.getAddress().equals("00:02:72:13:75:93"))
                                {
                                    Log.d("RemoteActivity", String.format("Connecting to %s",
                                            dev.getName()));

                                    /* Discover services via SDP */
                                    if (!dev.fetchUuidsWithSdp())
                                    {
                                        Log.d("RemoteActivity", "fetchUuidsWithSdp() failed");
                                    }

                                    /*
                                    btsocket = dev.createRfcommSocketToServiceRecord(
                                            UUID.fromString("00000000-0000-0000-0000-0000cdab0000"));
                                    */

                                    try
                                    {
                                        Method m = dev.getClass().getMethod(
                                                "createRfcommSocket", new Class[]{int.class});
                                        btsocket = (BluetoothSocket) m.invoke(dev, 1);
                                    }
                                    catch (InvocationTargetException ex)
                                    {
                                        ex.printStackTrace();
                                        btsocket = null;
                                    }
                                    catch (IllegalAccessException ex)
                                    {
                                        ex.printStackTrace();
                                        btsocket = null;
                                    }
                                    catch (NoSuchMethodException ex)
                                    {
                                        ex.printStackTrace();
                                        btsocket = null;
                                    }

                                    while (btdev.isDiscovering())
                                    {
                                        try
                                        {
                                            Thread.sleep(500);
                                        }
                                        catch (InterruptedException ex)
                                        {
                                            ex.printStackTrace();
                                        }
                                    }

                                    btsocket.connect();
                                }
                            }
                        }
                    }
                    else
                    {
                        Log.d("RemoteActivity", "No Bluetooth adapter!");
                    }
                }
                else
                {
                    Log.d("RemoteActivity", "Opening TCP socket");
                    InetAddress deviceAddress = InetAddress.getByName(prefs.getString("device", ""));
                    socket = new Socket(deviceAddress, SERVER_PORT);
                }
            }
            catch (UnknownHostException e1)
            {
                e1.printStackTrace();
            }
            catch (IOException e1)
            {
                if (socket != null)
                {
                    if (socket.isConnected())
                    {
                        try
                        {
                            socket.close();
                        }
                        catch (IOException e2)
                        {
                            e2.printStackTrace();
                        }
                    }
                }
                if (btsocket != null)
                {
                    if (btsocket.isConnected())
                    {
                        try
                        {
                            btsocket.close();
                        }
                        catch (IOException e2)
                        {
                            e2.printStackTrace();
                        }
                    }
                }
                e1.printStackTrace();
            }
        }
    }
}
