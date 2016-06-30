package com.mediabox.mediaboxremote;

import android.content.Context;
import android.content.Intent;
import android.graphics.Point;
import android.support.v7.app.AppCompatActivity;
import android.os.Bundle;
import android.util.Log;
import android.view.Display;
import android.view.KeyEvent;
import android.view.View;
import android.view.inputmethod.InputMethodManager;
import android.widget.Button;

import java.io.BufferedWriter;
import java.io.IOException;
import java.io.OutputStreamWriter;
import java.io.PrintWriter;
import java.net.InetAddress;
import java.net.Socket;
import java.net.UnknownHostException;

public class RemoteActivity extends AppCompatActivity
{
    private Socket socket = null;
    private static final int SERVER_PORT = 2048;
    private static final String SERVER_IP = "10.10.0.14";

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
    }

    private void openSocket()
    {
        closeSocket();
        new Thread(new ClientThread()).start();
    }

    private void sendMessage(String msg)
    {
        try {
            if (this.socket != null)
            {
                PrintWriter out = new PrintWriter(new BufferedWriter(
                        new OutputStreamWriter(socket.getOutputStream())), true);
                out.println(msg);
            }
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
        startService(new Intent(this, UDPListener.class));
        Log.d("Remote", "UDPListener started.");

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
    }

    @Override
    protected void onDestroy()
    {
        Log.d("Remote", "Destroying");
        stopService(new Intent(this, UDPListener.class));
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
        InputMethodManager im = (InputMethodManager) view.getContext().getSystemService(Context.INPUT_METHOD_SERVICE);
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
    }
    class ClientThread implements Runnable {
        @Override
        public void run() {
            try {
                InetAddress serverAddr = InetAddress.getByName(SERVER_IP);
                socket = new Socket(serverAddr, SERVER_PORT);
            } catch (UnknownHostException e1) {
                e1.printStackTrace();
            } catch (IOException e1) {
                e1.printStackTrace();
            }
        }
    }
}
