package com.mediabox.mediaboxremote;

import android.support.v7.app.AppCompatActivity;
import android.os.Bundle;
import android.view.View;

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
    }

    @Override
    protected void onStart()
    {
        super.onStart();
        this.openSocket();
    }

    @Override
    protected void onStop()
    {
        super.onStop();
        this.closeSocket();
    }

    public void onConnect(View view)
    {
        this.openSocket();
    }

    public void onButtonPressed(View view) {


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
        else if (view == this.findViewById(R.id.btnNext))
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
