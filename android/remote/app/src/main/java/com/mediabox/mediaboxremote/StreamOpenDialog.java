package com.mediabox.mediaboxremote;

import android.content.DialogInterface;
import android.util.Log;
import android.app.Dialog;
import android.os.Bundle;
import android.support.v7.app.AlertDialog;
import android.support.v7.app.AppCompatDialogFragment;
import android.view.Gravity;
import android.widget.CompoundButton;
import android.widget.LinearLayout;
import android.widget.RadioButton;
import android.widget.RadioGroup;
import android.widget.TableLayout;
import android.widget.TableRow;
import android.widget.TextView;

/**
 * Created by fernan on 11/4/17.
 */

public class StreamOpenDialog extends AppCompatDialogFragment {

    private String url;

    @Override
    public Dialog onCreateDialog(Bundle savedInstance)
    {
        final LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(
            LinearLayout.LayoutParams.FILL_PARENT, LinearLayout.LayoutParams.FILL_PARENT);
        final TableLayout layout = new TableLayout(getActivity());
        final TableRow title_row = new TableRow(getActivity());
        final TableRow spacer_row = new TableRow(getActivity());
        final TableRow options_row = new TableRow(getActivity());
        final TextView title = new TextView(getActivity());
        final RadioGroup options = new RadioGroup(getActivity());
        final RadioButton stream = new RadioButton(getActivity());
        final RadioButton download = new RadioButton(getActivity());

        stream.setChecked(true);
        title.setText("CHOOSE ACTION");
        stream.setText("STREAM");
        download.setText("DOWNLOAD");
        options.setOrientation(LinearLayout.HORIZONTAL);
        options.addView(stream);
        options.addView(download);
        layout.setLayoutParams(lp);
        title_row.setLayoutParams(lp);
        layout.setGravity(Gravity.CENTER | Gravity.BOTTOM);
        title_row.setGravity(Gravity.CENTER | Gravity.BOTTOM);
        options_row.setGravity(Gravity.CENTER | Gravity.BOTTOM);
        title_row.addView(title);
        options_row.addView(options);
        layout.addView(title_row);
        layout.addView(spacer_row);
        layout.addView(options_row);
        title.setTextSize(20.0f);
        spacer_row.setMinimumHeight(20);

        /* why we need this? */
        stream.setOnCheckedChangeListener(new CompoundButton.OnCheckedChangeListener() {
            @Override
            public void onCheckedChanged(CompoundButton buttonView, boolean isChecked) {
                download.setChecked(!isChecked);
            }
        });
        download.setOnCheckedChangeListener(new CompoundButton.OnCheckedChangeListener() {
            @Override
            public void onCheckedChanged(CompoundButton buttonView, boolean isChecked) {
                stream.setChecked(!isChecked);
            }
        });

        AlertDialog.Builder builder = new AlertDialog.Builder(getActivity());
        builder
                .setView(layout)
                .setPositiveButton("OK", new DialogInterface.OnClickListener() {
                    @Override
                    public void onClick(DialogInterface dialog, int which) {
                        ((RemoteActivity) getActivity()).onSendURL(StreamOpenDialog.this.url,
                                stream.isChecked() ? RemoteActivity.ACTION_STREAM :
                                    RemoteActivity.ACTION_DOWNLOAD);
                    }
                })
                .setNegativeButton("CANCEL", new DialogInterface.OnClickListener() {
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
