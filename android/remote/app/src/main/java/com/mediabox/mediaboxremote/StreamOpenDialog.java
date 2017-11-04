package com.mediabox.mediaboxremote;

import android.content.DialogInterface;
import android.util.Log;
import android.app.Dialog;
import android.os.Bundle;
import android.support.v7.app.AlertDialog;
import android.support.v7.app.AppCompatDialogFragment;

/**
 * Created by fernan on 11/4/17.
 */

public class StreamOpenDialog extends AppCompatDialogFragment {

    private String url;

    @Override
    public Dialog onCreateDialog(Bundle savedInstance)
    {
        AlertDialog.Builder builder = new AlertDialog.Builder(getActivity());
        builder.setMessage("Would you like to download this stream?")
                .setPositiveButton("YES", new DialogInterface.OnClickListener() {
                    @Override
                    public void onClick(DialogInterface dialog, int which) {
                        ((RemoteActivity) getActivity()).onSendURL(StreamOpenDialog.this.url);
                    }
                })
                .setNegativeButton("NO", new DialogInterface.OnClickListener() {
                    @Override
                    public void onClick(DialogInterface dialog, int which) {
                        Log.d("OpenStreamDialog", "Cancelling");
                    }
                });
        return builder.create();
    }

    public void setUrl(String url)
    {
        this.url = url;
    }
}
