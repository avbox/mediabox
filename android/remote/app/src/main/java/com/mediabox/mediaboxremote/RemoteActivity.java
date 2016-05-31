package com.mediabox.mediaboxremote;

import android.support.v7.app.AppCompatActivity;
import android.os.Bundle;
import android.util.Log;
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
    private Socket socket;
    private static final int SERVERPORT = 2047 + 1;
    private static final String SERVER_IP = "10.10.0.10";
    @Override
    protected void onCreate(Bundle savedInstanceState)
    {
        super.onCreate(savedInstanceState);
        setContentView(R.layout.activity_remote);
        new Thread(new ClientThread()).start();
    }

    private void sendMessage(String msg)
    {
        try {
            PrintWriter out = new PrintWriter(new BufferedWriter(
                    new OutputStreamWriter(socket.getOutputStream())),
                    true);
            out.println(msg);
        } catch (UnknownHostException e) {
            e.printStackTrace();
        } catch (IOException e) {
            e.printStackTrace();
        } catch (Exception e) {
            e.printStackTrace();
        }


    }

    public void onConnect(View view)
    {

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
                socket = new Socket(serverAddr, SERVERPORT);
            } catch (UnknownHostException e1) {
                e1.printStackTrace();
            } catch (IOException e1) {
                e1.printStackTrace();
            }
        }
    }
}
