package com.mediabox.mediaboxremote;

import android.app.Dialog;
import android.content.DialogInterface;
import android.os.Bundle;
import android.support.v7.app.AlertDialog;
import android.support.v7.app.AppCompatDialogFragment;
import android.util.Log;
import android.widget.EditText;
import android.widget.LinearLayout;

/**
 * Created by fernan on 11/1/17.
 */

public class SendURLDialogFragment extends AppCompatDialogFragment {

    private EditText input = null;
    private String url;

    @Override
    public Dialog onCreateDialog(Bundle savedInstance)
    {
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT,
                LinearLayout.LayoutParams.MATCH_PARENT);
        input = new EditText(getActivity());
        input.setLayoutParams(lp);
        if (url != null) {
            input.setText(url);
        }

        AlertDialog.Builder builder = new AlertDialog.Builder(getActivity());
        builder.setMessage("Enter URL")
                .setPositiveButton("Send", new DialogInterface.OnClickListener() {
                    @Override
                    public void onClick(DialogInterface dialog, int which) {
                        ((RemoteActivity) getActivity()).onSendURL(input.getText().toString());
                    }
                })
                .setNegativeButton("Cancel", new DialogInterface.OnClickListener() {
                    @Override
                    public void onClick(DialogInterface dialog, int which) {
                        Log.d("SendURLDialog", "Cancelling");
                    }
                })
                .setView(input);
        return builder.create();
    }

    public void setUrl(String url)
    {
        this.url = url;
    }

}
