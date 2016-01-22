﻿
## Caption2Ass_PCR_pf

ＴＳファイルから字幕を抽出します。


------------------------------------------------------------------
### 使い方

Run_Caption2Ass_PCR_pf.batにTSファイルをドロップ


### 使い方　　コマンドライン

ファイル  
Caption2Ass_PCR_pf.exe  -i "C:\video.ts"  -o "C:\video.ts"  -format srt

パイプ  
Caption2Ass_PCR_pf.exe  -pipe  -o "C:\video.ts"  -format srt



------------------------------------------------------------------
### 追加引数

    -p
    -pipe
パイプからデータを受けとる


    -limit 10.0
ファイル読込み速度を10.0 MiB/secに制限


    -NonCapTag
tsファイルに字幕が含まれてない場合に空のファイル .noncapを出力



------------------------------------------------------------------
### 引数

    -format srt
    -format ass
字幕ファイルの形式  
文字コード　UTF-8 bom  


    -detect_length 300
300×10,000パケットを探索して字幕が見つからなければプロセスを終了します。  
およそ３～５分間のデータ量に相当  
-detect_lengthが無いときの初期値は300  


その他の引数はソースコード内の   
"Caption2Ass_PCR_pf\readme history\Caption2Ass_PCR.exe の Readme.txt"  
に記載されています。


------------------------------------------------------------------
### Caption2Ass_PCRからの変更

出力ファイル名を少し変更  
-o "C:\video.ts" と指定されたら video.srtを出力していましたが、  
video.ts.srtを出力するように変更。.tsを自動で除去しないようにした。  
video.srtを出力するには -o "C:\video"と指定してください。  


------------------------------------------------------------------
### 謝辞
maki/maki_rxrzさん公開のCaption2Ass_PCR_20131011_Experimentalを元に作成しました。
オリジナル及び改良版の開発に関わった方々にお礼申し上げます。


------------------------------------------------------------------
### ライセンス
以下Caption2Ass_PCR.exe の Readme.txtより、

●EpgDataCap_Bon、TSEpgView_Sample、NetworkRemocon、Caption、TSEpgViewServerの
ソースの取り扱いについて
　特にGPLとかにはしないので自由に改変してもらって構わないです。
　改変して公開する場合は改変部分のソースぐらいは一緒に公開してください。
　（強制ではないので別に公開しなくてもいいです）
　EpgDataCap.dllの使い方の参考にしてもらうといいかも。

●EpgDataCap.dll、Caption.dllの取り扱いについて
　フリーソフトに組み込む場合は特に制限は設けません。ただし、dllはオリジナルのまま
　組み込んでください。
　このdllを使用したことによって発生した問題について保証は一切行いません。
　商用、シェアウェアなどに組み込むのは不可です。


