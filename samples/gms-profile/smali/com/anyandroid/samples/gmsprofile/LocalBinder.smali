# Stub AIDL minimal : une classe qui étend android.os.Binder (non implémentée par le pont).

.class public Lcom/anyandroid/samples/gmsprofile/LocalBinder;
.super Landroid/os/Binder;
.source "LocalBinder.java"

.method public constructor <init>()V
    .registers 1
    invoke-direct {p0}, Landroid/os/Binder;-><init>()V
    return-void
.end method

.method protected onTransact(ILandroid/os/Parcel;Landroid/os/Parcel;I)Z
    .registers 6
    invoke-virtual {p2}, Landroid/os/Parcel;->readInt()I
    invoke-super {p0, p1, p2, p3, p4}, Landroid/os/Binder;->onTransact(ILandroid/os/Parcel;Landroid/os/Parcel;I)Z
    move-result v0
    return v0
.end method
